#include <gtest/gtest.h>

#include <hffix.hpp>
#include <hffix_fields.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <span>
#include <string>
#include <vector>

using namespace std::literals::string_view_literals;
using namespace hffix;

TEST(HffixTest, basic) {
    char b[1024];
    message_writer w(b);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "A");
    ASSERT_TRUE(w.push_back_trailer());
    message_reader r(b);

    EXPECT_EQ(w.message_size(), r.message_size());

    message_reader::const_iterator i = r.begin();
    EXPECT_TRUE(r.find_with_hint(tag::MsgType, i));
    EXPECT_TRUE(i->value() == "A");
    EXPECT_TRUE(i->value() == (char const*)"A");
    EXPECT_TRUE(i->value() != "B");
    EXPECT_TRUE(i->value() == "A"sv);
    EXPECT_TRUE(i->value() != "B"sv);

    {
        message_writer w(b);
        EXPECT_FALSE(w.push_back_trailer<false>());
        EXPECT_TRUE(w.has_error());
    }
    {
        message_writer w(b);
        w.push_back_header("FIXT.1.1");
        w.push_back_header("FIXT.1.1");
        EXPECT_TRUE(w.has_error());
    }
}

template <typename F>
int test_bound_checking(F f) {
    char buffer[101];
    int minimum_size = 101;
    for (int i = 100; i >= 0; --i) {
        message_writer writer(buffer, buffer + i);
        buffer[i] = '\x55';
        f(writer);
        if (writer.ok()) {
            EXPECT_EQ(minimum_size, i + 1);
            minimum_size = i;
        }
        EXPECT_EQ(buffer[i], '\x55');
    }
    return minimum_size;
}

TEST(HffixTest, message_writer_bounds) {
    using W = message_writer;
    EXPECT_EQ(test_bound_checking([](W& w) { w.push_back_header("FIXT.1.1"); }), 20);
    EXPECT_EQ(test_bound_checking([](W& w) {
                  w.push_back_header("FIXT.1.1");
                  (void)w.push_back_trailer<false>();
              }),
              27);
    EXPECT_EQ(test_bound_checking([](W& w) {
                  w.push_back_header("FIXT.1.1");
                  (void)w.push_back_trailer<true>();
              }),
              27);

    auto const test_string = "string literal";
    EXPECT_EQ(
        test_bound_checking([&](W& w) { w.push_back_string(58, test_string, test_string + 14); }),
        27);
    EXPECT_EQ(test_bound_checking([&](W& w) { w.push_back_string(58, std::string(test_string)); }),
              27);
    EXPECT_EQ(
        test_bound_checking([&](W& w) { w.push_back_string(58, std::string_view(test_string)); }),
        27);
    EXPECT_EQ(test_bound_checking([&](W& w) { w.push_back_char(58, 'a'); }), 14);
    EXPECT_EQ(test_bound_checking([&](W& w) { w.push_back_int(58, 55); }), 24);
    EXPECT_EQ(test_bound_checking([&](W& w) { w.push_back_decimal(58, 123456, -3); }), 25);
    EXPECT_EQ(test_bound_checking([&](W& w) { w.push_back_decimal(58, 123456, 0); }), 25);
    EXPECT_EQ(test_bound_checking([&](W& w) { w.push_back_date(58, 1970, 1, 1); }), 21);
    EXPECT_EQ(test_bound_checking([&](W& w) { w.push_back_monthyear(58, 1970, 1); }), 19);
    EXPECT_EQ(test_bound_checking([&](W& w) { w.push_back_timeonly(58, 23, 59, 59, 999); }), 25);
    EXPECT_EQ(test_bound_checking([&](W& w) { w.push_back_timestamp(58, 1970, 1, 1, 23, 59, 59); }),
              30);
    EXPECT_EQ(
        test_bound_checking([&](W& w) { w.push_back_timestamp(58, 1970, 1, 1, 23, 59, 59, 999); }),
        34);
    EXPECT_EQ(
        test_bound_checking([&](W& w) { w.push_back_data(58, 59, test_string, test_string + 14); }),
        51);
}

static void test_checksum(message_writer& mw, char const (&expected)[4]) {
    ASSERT_TRUE(mw.push_back_trailer());
    char const* end = mw.message_end();

    EXPECT_EQ(end[-4], expected[0]);
    EXPECT_EQ(end[-3], expected[1]);
    EXPECT_EQ(end[-2], expected[2]);
}

TEST(HffixTest, checksum_empty) {
    char buffer[50] = {};
    message_writer writer(buffer);
    writer.push_back_header("FIXT.1.1");
    test_checksum(writer, "006");
}

TEST(HffixTest, checksum) {
    char buffer[50] = {};
    message_writer writer(buffer);
    writer.push_back_header("FIXT.1.1");
    writer.push_back_decimal(58, 123, 0);
    test_checksum(writer, "078");
}

TEST(HffixTest, checksum_negative) {
    char buffer[50] = {};
    message_writer writer(buffer);
    writer.push_back_header("FIXT.1.1");
    writer.push_back_char(58, '\x80');
    test_checksum(writer, "054");
}

TEST(HffixTest, checksum_calc) {
    char buffer[50] = {};
    message_writer writer(buffer);
    writer.push_back_header("FIXT.1.1");
    writer.push_back_string(tag::MsgType, "A");
    ASSERT_TRUE(writer.push_back_trailer());

    message_reader mr(writer);
    EXPECT_EQ(mr.calculate_check_sum(), mr.check_sum()->value().as_int_unchecked<unsigned char>());
}

TEST(HffixTest, null_field_value) {
    char buffer[50] = {};
    message_writer writer(buffer);
    writer.push_back_header("FIXT.1.1");
    writer.push_back_string(hffix::tag::MsgType, "A");
    writer.push_back_string(37, "");
    writer.push_back_string(38, "whatever");
    ASSERT_TRUE(writer.push_back_trailer());

    message_reader reader(writer);
    message_reader::const_iterator i = reader.begin();
    EXPECT_TRUE(reader.find_with_hint(38, i));
}

TEST(HffixTest, data_length) {
    char buffer[100] = {};
    message_writer writer(buffer);
    std::string datum("datum");
    writer.push_back_header("FIXT.1.1");
    writer.push_back_string(tag::MsgType, "A");
    writer.push_back_data(tag::RawDataLength, tag::RawData, &*datum.begin(), &*datum.end());
    writer.push_back_data(tag::EncodedUnderlyingProvisionTextLen,
                          tag::EncodedUnderlyingProvisionText,
                          &*datum.begin(),
                          &*datum.end());
    ASSERT_TRUE(writer.push_back_trailer());

    message_reader reader(writer);
    message_reader::const_iterator i;

    i = reader.begin();
    ASSERT_FALSE(reader.find_with_hint(tag::RawDataLength, i));

    i = reader.begin();
    ASSERT_FALSE(reader.find_with_hint(tag::EncodedUnderlyingProvisionTextLen, i));

    i = reader.begin();
    ASSERT_TRUE(reader.find_with_hint(tag::RawData, i));
    ASSERT_TRUE(i != reader.end());
    EXPECT_EQ(i->value().as_string_view(), datum);

    ASSERT_TRUE(reader.find_with_hint(tag::EncodedUnderlyingProvisionText, i));
    ASSERT_TRUE(i != reader.end());
    EXPECT_EQ(i->value().as_string_view(), datum);
}

TEST(HffixTest, iterating) {
    char b[1024];
    char* ptr = b;
    unsigned num = 0;
    for (size_t i = 0; i < 10; i++) {
        message_writer w(ptr, 1024 - (ptr - b));
        w.push_back_header("FIXT.1.1");
        w.push_back_string(tag::MsgType, "A");
        ASSERT_TRUE(w.push_back_trailer());
        ptr += w.message_size();
    }

    for (message_reader reader(b); reader.is_complete(); reader = reader.next_message_reader()) {
        if (reader.is_complete() && reader.is_valid()) {
            EXPECT_EQ(std::distance(reader.begin(), reader.end()), 1);
            num++;
        }
    }
    EXPECT_EQ(num, 10u);
}

TEST(HffixTest, msg_type_string) {
    EXPECT_EQ(std::string(msg_type::AccountSummaryReport), std::string("CQ"));
}

TEST(HffixTest, indexed_message_lookup) {
    char buffer[256];
    message_writer w(buffer);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_string(tag::SenderCompID, "ALICE");
    w.push_back_string(tag::TargetCompID, "BOB");
    w.push_back_int(tag::MsgSeqNum, 42);
    w.push_back_int(tag::OrderQty, 100);
    w.push_back_string(tag::Symbol, "AAPL");
    w.push_back_decimal(tag::Price, 50001, -2);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(w);
    ASSERT_TRUE(r.is_valid());

    hffix::field_index_buffer<32> idx_buffer;
    auto idx = hffix::build_field_index(r, idx_buffer);

    EXPECT_TRUE(idx.authoritative());
    EXPECT_FALSE(idx.truncated());
    EXPECT_FALSE(idx.overflowed());
    EXPECT_GE(idx.field_count(), 7u);

    std::size_t h1 = 0;
    EXPECT_EQ(idx.find_with_hint(tag::Symbol, h1).as_string_view(), "AAPL");
    std::size_t h2 = 0;
    EXPECT_EQ(idx.find_with_hint(tag::SenderCompID, h2).as_string_view(), "ALICE");
    std::size_t h3 = 0;
    EXPECT_EQ(idx.find_with_hint(tag::OrderQty, h3).as_int_unchecked<int>(), 100);
    std::size_t h4 = 0;
    EXPECT_EQ(idx.find_with_hint(tag::MsgSeqNum, h4).as_int_unchecked<int>(), 42);
    EXPECT_FALSE(idx.has(tag::ClOrdID));

    char buffer2[256];
    message_writer w2(buffer2);
    w2.push_back_header("FIXT.1.1");
    w2.push_back_string(tag::MsgType, "0");
    w2.push_back_string(tag::SenderCompID, "X");
    ASSERT_TRUE(w2.push_back_trailer());
    message_reader r2(w2);
    ASSERT_TRUE(r2.is_valid());

    auto idx2 = hffix::build_field_index(r2, idx_buffer);
    std::size_t h5 = 0;
    EXPECT_EQ(idx2.find_with_hint(tag::SenderCompID, h5).as_string_view(), "X");
    std::size_t h6 = 0;
    EXPECT_EQ(idx2.find_with_hint(tag::MsgType, h6).as_string_view(), "0");
    EXPECT_FALSE(idx2.has(tag::OrderQty));
}

TEST(HffixTest, chrono) {
    using namespace std::chrono;
    using TimePoint = time_point<system_clock, milliseconds>;

    TimePoint tsend(milliseconds(1502282096123ULL));

    milliseconds timeofday = hours(12) + minutes(34) + seconds(0) + milliseconds(789);

    char buffer[100] = {};
    message_writer writer(buffer);
    writer.push_back_header("FIXT.1.1");
    writer.push_back_string(hffix::tag::MsgType, "A");
    writer.push_back_timestamp(hffix::tag::SendingTime, tsend);
    writer.push_back_timeonly(hffix::tag::MDEntryTime, timeofday);
    ASSERT_TRUE(writer.push_back_trailer());

    message_reader reader(writer);
    message_reader::const_iterator i = reader.begin();
    ASSERT_TRUE(reader.find_with_hint(hffix::tag::SendingTime, i));
    EXPECT_EQ(i->value().as_string_view(), std::string("20170809-12:34:56.123"));

    TimePoint trecv;
    EXPECT_TRUE(i->value().as_timestamp(trecv));
    EXPECT_EQ(tsend, trecv);

    message_reader::const_iterator j = reader.begin();
    ASSERT_TRUE(reader.find_with_hint(hffix::tag::MDEntryTime, j));
    EXPECT_EQ(j->value().as_string_view(), std::string("12:34:00.789"));

    milliseconds todrecv;
    EXPECT_TRUE(j->value().as_timeonly(todrecv));
    EXPECT_EQ(timeofday, todrecv);
}

TEST(HffixTest, chrono_nano) {
    using namespace std::chrono;
    using TimePoint = time_point<system_clock, nanoseconds>;

    TimePoint tsend(nanoseconds(1502282096123456789ULL));

    nanoseconds timeofday = hours(12) + minutes(34) + seconds(0) + nanoseconds(789456123);

    char buffer[100] = {};
    message_writer writer(buffer);
    writer.push_back_header("FIXT.1.1");
    writer.push_back_string(hffix::tag::MsgType, "A");
    writer.push_back_timestamp_nano(hffix::tag::SendingTime, tsend);
    writer.push_back_timeonly_nano(hffix::tag::MDEntryTime, timeofday);
    ASSERT_TRUE(writer.push_back_trailer());

    message_reader reader(writer);
    message_reader::const_iterator i = reader.begin();
    ASSERT_TRUE(reader.find_with_hint(hffix::tag::SendingTime, i));
    EXPECT_EQ(i->value().as_string_view(), std::string("20170809-12:34:56.123456789"));

    TimePoint trecv;
    EXPECT_TRUE(i->value().as_timestamp_nano(trecv));
    EXPECT_EQ(tsend, trecv);

    message_reader::const_iterator j = reader.begin();
    ASSERT_TRUE(reader.find_with_hint(hffix::tag::MDEntryTime, j));
    EXPECT_EQ(j->value().as_string_view(), std::string("12:34:00.789456123"));

    nanoseconds todrecv;
    EXPECT_TRUE(j->value().as_timeonly_nano(todrecv));
    EXPECT_EQ(timeofday, todrecv);
}

TEST(HffixTest, header) {
    char const* begstr_cp = "FIXT.1.1";
    std::string_view begstr_sv = begstr_cp;

    char buffer_cp[100] = {};
    char buffer_sv[100] = {};

    message_writer writer_cp(buffer_cp);
    message_writer writer_sv(buffer_sv);

    writer_cp.push_back_header(begstr_cp);
    writer_sv.push_back_header(begstr_sv);

    ASSERT_EQ(writer_cp.message_size(), writer_sv.message_size());
    ASSERT_EQ(
        std::memcmp(writer_cp.message_begin(), writer_sv.message_begin(), writer_cp.message_size()),
        0);
}

TEST(HffixTest, read_string) {
    char buffer[100] = {};

    message_writer writer(buffer);

    writer.push_back_header("FIXT.1.1");
    writer.push_back_string(hffix::tag::MsgType, "A");
    ASSERT_TRUE(writer.push_back_trailer());

    message_reader reader(buffer);

    auto i = reader.begin();
    EXPECT_TRUE(reader.find_with_hint(tag::MsgType, i));
    EXPECT_TRUE(i->value() == "A");
    EXPECT_EQ(i->value().as_string_view(), "A");
}

TEST(HffixTest, epoch_nanos_roundtrip) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    using namespace std::chrono;
    auto const tp = sys_days{year{2024} / January / 15} + 9h + 30min + nanoseconds{123456789};
    std::int64_t const expected_nanos = tp.time_since_epoch().count();
    w.push_back_timestamp_epoch_nanos(tag::SendingTime, expected_nanos);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto it = r.begin();
    ASSERT_TRUE(r.find_with_hint(tag::SendingTime, it));
    auto parsed = it->value().as_epoch_nanos();
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, expected_nanos);
}

TEST(HffixTest, epoch_millis_roundtrip) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    using namespace std::chrono;
    auto const tp = sys_days{year{2024} / January / 15} + 9h + 30min + milliseconds{123};
    std::int64_t const expected_millis = tp.time_since_epoch().count();
    w.push_back_timestamp_epoch_millis(tag::SendingTime, expected_millis);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto it = r.begin();
    ASSERT_TRUE(r.find_with_hint(tag::SendingTime, it));
    auto parsed = it->value().as_epoch_millis();
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, expected_millis);
}

TEST(HffixTest, as_epoch_nanos_returns_nullopt_on_malformed) {
    char buf[64] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_string(tag::SendingTime, "garbage");
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto it = r.begin();
    ASSERT_TRUE(r.find_with_hint(tag::SendingTime, it));
    EXPECT_FALSE(it->value().as_epoch_nanos().has_value());
    EXPECT_FALSE(it->value().as_epoch_millis().has_value());
}

TEST(HffixTest, push_back_trailer_ok_path) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_int(tag::OrderQty, 100);
    EXPECT_TRUE(w.ok());
    EXPECT_TRUE(w.push_back_trailer());
    EXPECT_TRUE(w.ok());

    message_reader r(buf, w.message_end());
    EXPECT_TRUE(r.is_complete());
    EXPECT_TRUE(r.is_valid());
}

TEST(HffixTest, try_write_message_success) {
    char buf[256] = {};
    char* end = nullptr;
    bool ok = try_write_message(buf, buf + sizeof(buf), end, [](message_writer& w) {
        w.push_back_header("FIXT.1.1");
        w.push_back_string(tag::MsgType, "D");
        w.push_back_int(tag::OrderQty, 100);
    });
    ASSERT_TRUE(ok);

    message_reader r(buf, end);
    EXPECT_TRUE(r.is_complete());
    EXPECT_TRUE(r.is_valid());
}

TEST(HffixTest, try_write_message_overflow) {
    char buf[20] = {};
    char* end = nullptr;
    bool ok = try_write_message(buf, buf + sizeof(buf), end, [](message_writer& w) {
        w.push_back_header("FIXT.1.1");
        w.push_back_string(tag::MsgType, "D");
        w.push_back_string(tag::SenderCompID, "AAAAAAAA");
    });
    EXPECT_FALSE(ok);
}

TEST(HffixTest, group_iteration) {
    char buf[1024] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "W");
    w.push_back_int(tag::NoMDEntries, 3);
    for (int i = 0; i < 3; ++i) {
        w.push_back_char(tag::MDEntryType, '0' + char(i));
        w.push_back_decimal(tag::MDEntryPx, 10000 + i, -2);
        w.push_back_int(tag::MDEntrySize, 100 * (i + 1));
    }
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    auto md = r.group(tag::NoMDEntries, tag::MDEntryType);
    EXPECT_EQ(md.size(), 3u);

    int seen = 0;
    for (auto const& entry : md) {
        auto it = entry.begin();
        ASSERT_TRUE(entry.find_with_hint(tag::MDEntryType, it));
        EXPECT_EQ(it->value().as_char(), char('0' + seen));

        it = entry.begin();
        ASSERT_TRUE(entry.find_with_hint(tag::MDEntrySize, it));
        EXPECT_EQ(it->value().as_int_unchecked<int>(), 100 * (seen + 1));

        ++seen;
    }
    EXPECT_EQ(seen, 3);
}

TEST(HffixTest, message_writer_noexcept_overflow) {
    char buf[20] = {};
    message_writer w(buf);
    static_assert(noexcept(w.push_back_header("FIXT.1.1")));
    static_assert(noexcept(w.push_back_string(tag::MsgType, "D")));
    static_assert(noexcept(w.push_back_trailer()));

    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_string(tag::SenderCompID, "AAAAAAAA");
    EXPECT_FALSE(w.ok());
    EXPECT_TRUE(w.has_error());
    EXPECT_FALSE(w.push_back_trailer());
}

TEST(HffixTest, message_writer_happy_path) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_int(tag::OrderQty, 100);
    EXPECT_TRUE(w.ok());
    EXPECT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_complete());
    ASSERT_TRUE(r.is_valid());
}

TEST(HffixTest, group_compile_time_dispatch) {
    char buf[1024] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "W");
    w.push_back_int(tag::NoMDEntries, 2);
    w.push_back_char(tag::MDEntryType, '0');
    w.push_back_decimal(tag::MDEntryPx, 100, -2);
    w.push_back_char(tag::MDEntryType, '1');
    w.push_back_decimal(tag::MDEntryPx, 200, -2);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    static_assert(hffix::groups::group_def<tag::NoMDEntries>::first_tag == tag::MDEntryType);
    auto md = r.group<tag::NoMDEntries>();
    EXPECT_EQ(md.size(), 2u);
    int seen = 0;
    for (auto const& entry : md) {
        (void)entry;
        ++seen;
    }
    EXPECT_EQ(seen, 2);
}

TEST(HffixTest, group_iteration_respects_count) {
    char buf[1024] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "W");
    w.push_back_int(tag::NoMDEntries, 2);
    w.push_back_char(tag::MDEntryType, '0');
    w.push_back_decimal(tag::MDEntryPx, 10000, -2);
    w.push_back_int(tag::MDEntrySize, 100);
    w.push_back_char(tag::MDEntryType, '1');
    w.push_back_decimal(tag::MDEntryPx, 20000, -2);
    w.push_back_int(tag::MDEntrySize, 200);
    w.push_back_char(tag::MDEntryType, '2');
    w.push_back_decimal(tag::MDEntryPx, 30000, -2);
    w.push_back_int(tag::MDEntrySize, 300);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    auto md = r.group(tag::NoMDEntries, tag::MDEntryType);
    EXPECT_EQ(md.size(), 2u);

    std::vector<char> seen_types;
    for (auto const& entry : md) {
        auto it = entry.begin();
        ASSERT_TRUE(entry.find_with_hint(tag::MDEntryType, it));
        seen_types.push_back(it->value().as_char());
    }
    ASSERT_EQ(seen_types.size(), 2u);
    EXPECT_EQ(seen_types[0], '0');
    EXPECT_EQ(seen_types[1], '1');
}

TEST(HffixTest, group_zero_entries) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "W");
    w.push_back_int(tag::NoMDEntries, 0);
    w.push_back_string(tag::Symbol, "AAPL");
    w.push_back_int(tag::OrderQty, 42);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    auto md = r.group(tag::NoMDEntries, tag::MDEntryType);
    EXPECT_EQ(md.size(), 0u);
    EXPECT_TRUE(md.empty());
    int seen = 0;
    for (auto const& e : md) {
        (void)e;
        ++seen;
    }
    EXPECT_EQ(seen, 0);

    auto it = r.begin();
    ASSERT_TRUE(r.find_with_hint(tag::Symbol, it));
    EXPECT_EQ(it->value().as_string_view(), "AAPL");
    it = r.begin();
    ASSERT_TRUE(r.find_with_hint(tag::OrderQty, it));
    EXPECT_EQ(it->value().as_int_unchecked<int>(), 42);
}

TEST(HffixTest, group_compile_runtime_match) {
    char buf[1024] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "W");
    w.push_back_int(tag::NoMDEntries, 3);
    for (int i = 0; i < 3; ++i) {
        w.push_back_char(tag::MDEntryType, '0' + char(i));
        w.push_back_decimal(tag::MDEntryPx, 10000 + i, -2);
        w.push_back_int(tag::MDEntrySize, 100 * (i + 1));
    }
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    auto ct = r.group<tag::NoMDEntries>();
    auto rt = r.group(tag::NoMDEntries, tag::MDEntryType);
    ASSERT_EQ(ct.size(), rt.size());

    std::vector<int> ct_tags, rt_tags;
    for (auto const& e : ct)
        for (auto it = e.begin(); it != e.end(); ++it)
            ct_tags.push_back(it->tag());
    for (auto const& e : rt)
        for (auto it = e.begin(); it != e.end(); ++it)
            rt_tags.push_back(it->tag());
    EXPECT_EQ(ct_tags, rt_tags);
}

TEST(HffixTest, group_nested_compile_time_dispatch) {
    char buf[1024] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "AN");
    w.push_back_int(tag::NoAsgnReqs, 1);
    w.push_back_int(tag::NoPartyIDs, 2);
    w.push_back_string(tag::PartyID, "ALICE");
    w.push_back_string(tag::PartyID, "BOB");
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    auto outer = r.group<tag::NoAsgnReqs>();
    ASSERT_EQ(outer.size(), 1u);
    auto it = outer.begin();
    auto inner_ct = (*it).template group<tag::NoPartyIDs>();
    auto inner_rt = (*it).group(tag::NoPartyIDs, tag::PartyID);

    EXPECT_EQ(inner_ct.size(), 2u);
    EXPECT_EQ(inner_rt.size(), 2u);

    std::vector<std::string> ct_ids, rt_ids;
    for (auto const& e : inner_ct) {
        auto pit = e.begin();
        ASSERT_TRUE(e.find_with_hint(tag::PartyID, pit));
        ct_ids.emplace_back(pit->value().as_string_view());
    }
    for (auto const& e : inner_rt) {
        auto pit = e.begin();
        ASSERT_TRUE(e.find_with_hint(tag::PartyID, pit));
        rt_ids.emplace_back(pit->value().as_string_view());
    }
    EXPECT_EQ(ct_ids, (std::vector<std::string>{"ALICE", "BOB"}));
    EXPECT_EQ(ct_ids, rt_ids);
}

TEST(HffixTest, group_empty_when_absent) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "0");
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    auto g = r.group(tag::NoMDEntries, tag::MDEntryType);
    EXPECT_EQ(g.size(), 0u);
    EXPECT_TRUE(g.empty());
    EXPECT_EQ(g.begin(), g.end());
}

TEST(HffixTest, try_as_int_validates) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_int(tag::OrderQty, 100);
    w.push_back_string(tag::Symbol, "AAPL");
    w.push_back_string(tag::Account, "12X");
    w.push_back_string(tag::Text, "");
    w.push_back_string(tag::ClOrdID, "-42");
    w.push_back_string(tag::SecurityID, "+5");
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    auto find = [&](int t) {
        auto it = r.begin();
        EXPECT_TRUE(r.find_with_hint(t, it));
        return it;
    };

    {
        int out = -1;
        EXPECT_TRUE(find(tag::OrderQty)->value().try_as_int(out));
        EXPECT_EQ(out, 100);
    }
    {
        int out = -1;
        EXPECT_FALSE(find(tag::Symbol)->value().try_as_int(out));
        EXPECT_EQ(out, -1);
    }
    {
        int out = -1;
        EXPECT_FALSE(find(tag::Account)->value().try_as_int(out));
    }
    {
        int out = -1;
        EXPECT_FALSE(find(tag::Text)->value().try_as_int(out));
    }
    {
        int out = 0;
        EXPECT_TRUE(find(tag::ClOrdID)->value().try_as_int(out));
        EXPECT_EQ(out, -42);
    }
    {
        unsigned out = 0;
        EXPECT_FALSE(find(tag::ClOrdID)->value().try_as_int(out));
    }
    {
        int out = 0;
        EXPECT_FALSE(find(tag::SecurityID)->value().try_as_int(out));
    }
}

TEST(HffixTest, try_as_decimal_validates) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_decimal(tag::Price, 50001, -2);
    w.push_back_string(tag::Symbol, "1.2.3");
    w.push_back_string(tag::Account, "abc");
    w.push_back_string(tag::Text, ".");
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    auto find = [&](int t) {
        auto it = r.begin();
        EXPECT_TRUE(r.find_with_hint(t, it));
        return it;
    };

    {
        int m = 0, e = 0;
        EXPECT_TRUE(find(tag::Price)->value().try_as_decimal(m, e));
        EXPECT_EQ(m, 50001);
        EXPECT_EQ(e, -2);
    }
    {
        int m = -1, e = -1;
        EXPECT_FALSE(find(tag::Symbol)->value().try_as_decimal(m, e));
    }
    {
        int m = -1, e = -1;
        EXPECT_FALSE(find(tag::Account)->value().try_as_decimal(m, e));
    }
    {
        int m = -1, e = -1;
        EXPECT_FALSE(find(tag::Text)->value().try_as_decimal(m, e));
    }
}

TEST(HffixTest, length_tag_index_full_table) {
    constexpr hffix::details::length_tag_index t{};

    constexpr auto n = sizeof(::length_fields) / sizeof(::length_fields[0]);
    for (std::size_t i = 0; i < n; ++i) {
        int const tg = ::length_fields[i];
        EXPECT_TRUE(t.contains(tg)) << "missing length tag " << tg;
        EXPECT_TRUE(hffix::details::is_tag_a_data_length(tg))
            << "is_tag_a_data_length(" << tg << ") false";
    }

    // Tag above the 1024 bitmap split.
    static_assert(t.contains(hffix::tag::DerivativeEncodedIssuerLen));

    EXPECT_FALSE(t.contains(hffix::tag::BodyLength));
    EXPECT_FALSE(t.contains(hffix::tag::MsgType));
    EXPECT_FALSE(t.contains(hffix::tag::CheckSum));
    EXPECT_FALSE(t.contains(hffix::tag::OrderQty));
    EXPECT_FALSE(t.contains(hffix::tag::UnderlyingLegSymbol));
    EXPECT_FALSE(hffix::details::is_tag_a_data_length(hffix::tag::UnderlyingLegSymbol));
}

TEST(HffixTest, indexed_group_entry_lookup) {
    char buf[1024] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "W");
    w.push_back_int(tag::NoMDEntries, 2);
    w.push_back_char(tag::MDEntryType, '0');
    w.push_back_decimal(tag::MDEntryPx, 10000, -2);
    w.push_back_int(tag::MDEntrySize, 500);
    w.push_back_char(tag::MDEntryType, '1');
    w.push_back_decimal(tag::MDEntryPx, 10050, -2);
    w.push_back_int(tag::MDEntrySize, 300);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    hffix::field_index_buffer<32> entry_idx;
    int seen = 0;
    for (auto const& entry : r.group(tag::NoMDEntries, tag::MDEntryType)) {
        auto idx = hffix::build_field_index(entry, entry_idx);
        EXPECT_TRUE(idx.authoritative());

        std::size_t hh1 = 0;
        auto type_field = idx.find_with_hint(tag::MDEntryType, hh1);
        ASSERT_NE(type_field.begin(), type_field.end());
        EXPECT_EQ(type_field.as_char(), char('0' + seen));

        int m = 0, e = 0;
        std::size_t hh2 = 0;
        ASSERT_TRUE(idx.find_with_hint(tag::MDEntryPx, hh2).try_as_decimal(m, e));
        EXPECT_EQ(m, seen == 0 ? 10000 : 10050);
        EXPECT_EQ(e, -2);

        std::size_t hh3 = 0;
        EXPECT_EQ(idx.find_with_hint(tag::MDEntrySize, hh3).as_int_unchecked<int>(),
                  seen == 0 ? 500 : 300);
        EXPECT_FALSE(idx.has(tag::Symbol));
        ++seen;
    }
    EXPECT_EQ(seen, 2);
}

TEST(HffixTest, indexed_message_handles_large_message_above_64k) {
    constexpr std::size_t kBig = 70 * 1024;
    std::vector<char> buf(kBig, 'A');
    message_writer w(buf.data(), buf.data() + kBig);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "B");
    std::string filler(64 * 1024, 'X');
    w.push_back_string(tag::Text, filler);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf.data(), w.message_end());
    ASSERT_TRUE(r.is_valid());
    ASSERT_GT(r.message_size(), 0xFFFFu);

    hffix::field_index_buffer<32> idx_buffer;
    auto idx = hffix::build_field_index(r, idx_buffer);
    EXPECT_FALSE(idx.overflowed());
    EXPECT_FALSE(idx.truncated());
    EXPECT_TRUE(idx.authoritative());
    EXPECT_GT(idx.field_count(), 0u);

    // Round-trip the large Text field through value_at.
    std::size_t hint = 0;
    auto v = idx.find_with_hint(tag::Text, hint);
    EXPECT_EQ(v.size(), filler.size());
    EXPECT_EQ(v.as_string_view(), filler);
}

TEST(HffixTest, indexed_message_truncated) {
    char buf[2048] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    for (int i = 0; i < 16; ++i) {
        std::string val = "v" + std::to_string(i);
        w.push_back_string(10000 + i, val);
    }
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    hffix::field_index_buffer<8> idx_buffer;
    auto idx = hffix::build_field_index(r, idx_buffer);
    EXPECT_TRUE(idx.truncated());
    EXPECT_FALSE(idx.overflowed());
    EXPECT_FALSE(idx.authoritative());
    EXPECT_EQ(idx.field_count(), 8u);

    std::vector<std::pair<int, std::string>> first_n;
    for (auto it = r.begin(); it != r.end() && first_n.size() < 8; ++it) {
        first_n.emplace_back(it->tag(), std::string(it->value().as_string_view()));
    }
    ASSERT_EQ(first_n.size(), 8u);
    for (auto const& [tg, val] : first_n) {
        EXPECT_TRUE(idx.has(tg)) << "missing tag " << tg;
        std::size_t h = 0;
        EXPECT_EQ(idx.find_with_hint(tg, h).as_string_view(), val);
    }
}

TEST(HffixTest, checksum_roundtrip) {
    char buf[512] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_string(tag::SenderCompID, "ALICE");
    w.push_back_string(tag::TargetCompID, "BOB");
    w.push_back_int(tag::MsgSeqNum, 101);
    w.push_back_string(tag::ClOrdID, "ORD-XYZ-001");
    w.push_back_char(tag::Side, '1');
    w.push_back_string(tag::Symbol, "MSFT");
    w.push_back_string(tag::SecurityID, "US5949181045");
    w.push_back_int(tag::OrderQty, 500);
    w.push_back_decimal(tag::Price, 41523, -2);
    w.push_back_char(tag::OrdType, '2');
    w.push_back_string(tag::TimeInForce, "0");
    w.push_back_string(tag::Account, "ACCT-7");
    w.push_back_string(tag::Text, "vectorized checksum smoke test");
    ASSERT_TRUE(w.template push_back_trailer<true>());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    unsigned const written = r.check_sum()->value().as_int_unchecked<unsigned>();
    unsigned const computed = r.calculate_check_sum();
    EXPECT_EQ(written, computed);
    EXPECT_LT(written, 256u);
}

TEST(HffixTest, reader_copy_preserves_state_without_reparse) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_int(tag::MsgSeqNum, 42);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    ASSERT_TRUE(r.is_complete());

    message_reader copy = r;
    EXPECT_EQ(copy.is_valid(), r.is_valid());
    EXPECT_EQ(copy.is_complete(), r.is_complete());
    EXPECT_EQ(copy.message_begin(), r.message_begin());
    EXPECT_EQ(copy.message_end(), r.message_end());
    EXPECT_EQ(copy.message_size(), r.message_size());
    EXPECT_EQ(copy.begin()->tag(), r.begin()->tag());
    EXPECT_EQ(copy.calculate_check_sum(), r.calculate_check_sum());

    message_reader assigned(buf, buf);
    assigned = r;
    EXPECT_EQ(assigned.is_valid(), r.is_valid());
    EXPECT_EQ(assigned.message_end(), r.message_end());
    EXPECT_EQ(assigned.begin()->tag(), r.begin()->tag());

    static_assert(std::is_nothrow_copy_constructible_v<message_reader>);
    static_assert(std::is_nothrow_copy_assignable_v<message_reader>);
    static_assert(std::is_nothrow_move_constructible_v<message_reader>);
    static_assert(std::is_nothrow_move_assignable_v<message_reader>);
}

namespace {
template <class T>
[[nodiscard]] bool parse_int(std::string_view s, T& out) {
    return ::hffix::details::try_atoi<T>(s.data(), s.data() + s.size(), out);
}

template <class T>
[[nodiscard]] bool parse_uint(std::string_view s, T& out) {
    return ::hffix::details::try_atou<T>(s.data(), s.data() + s.size(), out);
}

template <class T>
[[nodiscard]] bool parse_dec(std::string_view s, T& m, T& e) {
    return ::hffix::details::try_atod<T>(s.data(), s.data() + s.size(), m, e);
}
}  // namespace

TEST(HffixTest, try_atoi_overflow_rejected_no_ub) {
    int64_t v = 0;
    EXPECT_TRUE(parse_int<int64_t>("9223372036854775807", v));
    EXPECT_EQ(v, std::numeric_limits<int64_t>::max());

    EXPECT_TRUE(parse_int<int64_t>("-9223372036854775808", v));
    EXPECT_EQ(v, std::numeric_limits<int64_t>::min());

    EXPECT_FALSE(parse_int<int64_t>("9223372036854775808", v));  // INT64_MAX + 1
    EXPECT_FALSE(parse_int<int64_t>("-9223372036854775809", v));
    EXPECT_FALSE(parse_int<int64_t>("99999999999999999999", v));

    int32_t w = 0;
    EXPECT_TRUE(parse_int<int32_t>("2147483647", w));
    EXPECT_EQ(w, std::numeric_limits<int32_t>::max());
    EXPECT_TRUE(parse_int<int32_t>("-2147483648", w));
    EXPECT_EQ(w, std::numeric_limits<int32_t>::min());
    EXPECT_FALSE(parse_int<int32_t>("2147483648", w));
    EXPECT_FALSE(parse_int<int32_t>("-2147483649", w));
}

TEST(HffixTest, try_atou_overflow_rejected) {
    uint64_t v = 0;
    EXPECT_TRUE(parse_uint<uint64_t>("18446744073709551615", v));
    EXPECT_EQ(v, std::numeric_limits<uint64_t>::max());
    EXPECT_FALSE(parse_uint<uint64_t>("18446744073709551616", v));
    EXPECT_FALSE(parse_uint<uint64_t>("99999999999999999999", v));

    uint32_t w = 0;
    EXPECT_TRUE(parse_uint<uint32_t>("4294967295", w));
    EXPECT_FALSE(parse_uint<uint32_t>("4294967296", w));
}

TEST(HffixTest, try_atod_overflow_rejected_no_ub) {
    int64_t m = 0, e = 0;
    EXPECT_TRUE(parse_dec<int64_t>("9223372036854775807", m, e));
    EXPECT_EQ(m, std::numeric_limits<int64_t>::max());
    EXPECT_EQ(e, 0);

    EXPECT_TRUE(parse_dec<int64_t>("-9223372036854775808", m, e));
    EXPECT_EQ(m, std::numeric_limits<int64_t>::min());
    EXPECT_EQ(e, 0);

    EXPECT_TRUE(parse_dec<int64_t>("123.45", m, e));
    EXPECT_EQ(m, 12345);
    EXPECT_EQ(e, -2);

    EXPECT_FALSE(parse_dec<int64_t>("92233720368547758.08", m, e));  // mantissa overflows
    EXPECT_FALSE(parse_dec<int64_t>("1.2.3", m, e));                 // two dots
    EXPECT_FALSE(parse_dec<int64_t>(".", m, e));                     // no digits
    EXPECT_FALSE(parse_dec<int64_t>("-", m, e));
    EXPECT_FALSE(parse_dec<int64_t>("", m, e));
}

TEST(HffixTest, as_epoch_nanos_rejects_out_of_range_year_no_overflow) {
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "0");
    w.push_back_string(tag::SendingTime, "99999999-00:00:00.000000000");
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto it = r.begin();
    ASSERT_TRUE(r.find_with_hint(tag::SendingTime, it));
    EXPECT_FALSE(it->value().as_epoch_nanos().has_value());
    EXPECT_FALSE(it->value().as_epoch_millis().has_value());
}

TEST(HffixTest, increment_terminates_on_malformed_data_length_field) {
    char const wire[] =
        "8=FIX.4.2\x01"
        "9=000075\x01"
        "35=n\x01"
        "49=ALICE\x01"
        "56=BOB\x01"
        "34=1\x01"
        "52=20240115-09:30:00.000\x01"
        "95=14\x01"
        "96:\x01hello\x01world\x02\x03\x01"
        "10=241\x01";
    auto const wire_len = sizeof(wire) - 1;

    message_reader r(wire, wire + wire_len);
    if (!r.is_valid() || !r.is_complete())
        return;
    std::size_t field_count = 0;
    for (auto it = r.begin(); it != r.end(); ++it) {
        if (++field_count > 1024) {
            FAIL() << "iterator did not terminate on malformed data-length field";
        }
    }
}

TEST(HffixTest, calculate_check_sum_callable_via_const_ref) {
    char buf[64] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "A");
    ASSERT_TRUE(w.push_back_trailer());

    message_reader const r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());
    auto const ck = r.calculate_check_sum();
    EXPECT_EQ(ck, r.check_sum()->value().as_int_unchecked<unsigned char>());
    static_assert(noexcept(r.calculate_check_sum()));
}

TEST(HffixTest, group_overstated_count_does_not_ub) {
    // NoMDEntries=5 on wire, 2 entries in body; iteration stops at 2.
    char buf[1024] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "W");
    w.push_back_int(tag::NoMDEntries, 5);
    w.push_back_char(tag::MDEntryType, '0');
    w.push_back_decimal(tag::MDEntryPx, 10000, -2);
    w.push_back_int(tag::MDEntrySize, 100);
    w.push_back_char(tag::MDEntryType, '1');
    w.push_back_decimal(tag::MDEntryPx, 20000, -2);
    w.push_back_int(tag::MDEntrySize, 200);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    auto md = r.group(tag::NoMDEntries, tag::MDEntryType);
    std::vector<char> seen_types;
    for (auto const& entry : md) {
        auto it = entry.begin();
        ASSERT_TRUE(entry.find_with_hint(tag::MDEntryType, it));
        seen_types.push_back(it->value().as_char());
    }
    ASSERT_EQ(seen_types.size(), 2u);
    EXPECT_EQ(seen_types[0], '0');
    EXPECT_EQ(seen_types[1], '1');
}

TEST(HffixTest, group_garbage_count_returns_empty) {
    // Non-numeric NoMDEntries; group() returns empty.
    char buf[256] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "W");
    w.push_back_string(tag::NoMDEntries, "abc");
    w.push_back_char(tag::MDEntryType, '0');
    w.push_back_decimal(tag::MDEntryPx, 10000, -2);
    ASSERT_TRUE(w.push_back_trailer());

    message_reader r(buf, w.message_end());
    ASSERT_TRUE(r.is_valid());

    auto md = r.group(tag::NoMDEntries, tag::MDEntryType);
    EXPECT_TRUE(md.empty());
    EXPECT_EQ(md.size(), 0u);
    int seen = 0;
    for (auto const& e : md) {
        (void)e;
        ++seen;
    }
    EXPECT_EQ(seen, 0);
}

namespace {

struct msg_buffer {
    char buf[8192]{};
    char* cursor = buf;

    void append(std::string_view msg_type, int seq) {
        message_writer w(cursor, buf + sizeof(buf));
        w.push_back_header("FIXT.1.1");
        w.push_back_string(tag::MsgType, msg_type);
        w.push_back_int(tag::MsgSeqNum, seq);
        if (w.push_back_trailer())
            cursor = w.message_end();
    }

    char const* data() const noexcept { return buf; }

    std::size_t size() const noexcept { return static_cast<std::size_t>(cursor - buf); }

    char const* end() const noexcept { return cursor; }
};

}  // namespace

TEST(HffixTest, messages_range_empty_buffer) {
    char buf[8] = {};
    auto rng = messages(buf, buf);
    int seen = 0;
    for (auto const& r : rng) {
        (void)r;
        ++seen;
    }
    EXPECT_EQ(seen, 0);
    EXPECT_EQ(rng.begin().remainder(), buf);
}

TEST(HffixTest, messages_range_single_message) {
    msg_buffer mb;
    mb.append("A", 1);

    auto rng = messages(mb.data(), mb.size());
    int seen = 0;
    int last_seq = 0;
    for (auto const& r : rng) {
        ASSERT_TRUE(r.is_complete());
        ASSERT_TRUE(r.is_valid());
        auto it = r.begin();
        ASSERT_TRUE(r.find_with_hint(tag::MsgSeqNum, it));
        ASSERT_TRUE(it->value().try_as_int(last_seq));
        ++seen;
    }
    EXPECT_EQ(seen, 1);
    EXPECT_EQ(last_seq, 1);

    auto it = rng.begin();
    while (it != rng.end())
        ++it;
    EXPECT_EQ(it.remainder(), mb.end());
}

TEST(HffixTest, messages_range_three_concatenated) {
    msg_buffer mb;
    mb.append("A", 1);
    mb.append("0", 2);
    mb.append("D", 3);

    std::vector<int> seqs;
    auto rng = messages(mb.data(), mb.size());
    for (auto const& r : rng) {
        ASSERT_TRUE(r.is_valid());
        auto it = r.begin();
        ASSERT_TRUE(r.find_with_hint(tag::MsgSeqNum, it));
        int s = 0;
        ASSERT_TRUE(it->value().try_as_int(s));
        seqs.push_back(s);
    }
    EXPECT_EQ(seqs, (std::vector<int>{1, 2, 3}));

    auto it = rng.begin();
    while (it != rng.end())
        ++it;
    EXPECT_EQ(it.remainder(), mb.end());
}

TEST(HffixTest, messages_range_truncated_tail) {
    msg_buffer mb;
    mb.append("A", 1);
    mb.append("0", 2);
    char const* second_msg_end = mb.end();
    mb.append("D", 3);
    char const* third_msg_end = mb.end();

    // Strip last 5 bytes (mid-message): tail must be the start of msg #3.
    std::size_t truncated_len = mb.size() - 5;
    char const* truncated_end = mb.data() + truncated_len;

    std::vector<int> seqs;
    auto rng = messages(mb.data(), truncated_end);
    auto it = rng.begin();
    for (; it != rng.end(); ++it) {
        int s = 0;
        auto fit = it->begin();
        ASSERT_TRUE(it->find_with_hint(tag::MsgSeqNum, fit));
        ASSERT_TRUE(fit->value().try_as_int(s));
        seqs.push_back(s);
    }
    EXPECT_EQ(seqs, (std::vector<int>{1, 2}));
    EXPECT_EQ(it.remainder(), second_msg_end);
    EXPECT_NE(it.remainder(), third_msg_end);
}

TEST(HffixTest, for_each_message_returns_tail) {
    msg_buffer mb;
    mb.append("A", 7);
    mb.append("0", 8);
    char const* full_tail = mb.end();

    std::vector<int> seqs;
    char const* tail = for_each_message(mb.data(), full_tail, [&](message_reader const& r) {
        ASSERT_TRUE(r.is_valid());
        auto it = r.begin();
        ASSERT_TRUE(r.find_with_hint(tag::MsgSeqNum, it));
        int s = 0;
        ASSERT_TRUE(it->value().try_as_int(s));
        seqs.push_back(s);
    });
    EXPECT_EQ(seqs, (std::vector<int>{7, 8}));
    EXPECT_EQ(tail, full_tail);
}

TEST(HffixTest, for_each_message_partial_tail_pointer) {
    msg_buffer mb;
    mb.append("A", 1);
    char const* msg1_end = mb.end();
    mb.append("D", 2);

    std::size_t partial = mb.size() - 3;
    char const* partial_end = mb.data() + partial;

    int count = 0;
    char const* tail =
        for_each_message(mb.data(), partial_end, [&](message_reader const&) { ++count; });
    EXPECT_EQ(count, 1);
    EXPECT_EQ(tail, msg1_end);
}

TEST(HffixTest, messages_range_skips_invalid_with_resync) {
    msg_buffer mb;
    mb.append("A", 1);
    // Splat random bytes into the middle, then append more valid messages.
    // The reader will hit a bad frame after msg 1, then next_message_reader
    // resyncs by scanning forward for "8=FIX".
    std::memcpy(mb.cursor, "garbage_bytes_not_a_fix_frame", 29);
    mb.cursor += 29;
    mb.append("D", 2);

    std::vector<int> seqs;
    for (auto const& r : messages(mb.data(), mb.size())) {
        auto it = r.begin();
        ASSERT_TRUE(r.find_with_hint(tag::MsgSeqNum, it));
        int s = 0;
        ASSERT_TRUE(it->value().try_as_int(s));
        seqs.push_back(s);
    }
    EXPECT_EQ(seqs, (std::vector<int>{1, 2}));
}

namespace {
struct corruption_outcome {
    bool complete;
    bool valid;
    std::size_t fields_seen;
};

corruption_outcome probe(char const* buf, std::size_t n) {
    message_reader r(buf, buf + n);
    corruption_outcome out{r.is_complete(), r.is_valid(), 0};
    if (!r.is_valid() || !r.is_complete())
        return out;
    for (auto it = r.begin(); it != r.end(); ++it) {
        if (++out.fields_seen > 4096) {
            ADD_FAILURE() << "iterator failed to terminate on corrupted message";
            break;
        }
    }
    return out;
}
}  // namespace

TEST(StateMachineCorruption, empty_buffer) {
    auto o = probe("", 0);
    EXPECT_FALSE(o.complete);
    EXPECT_EQ(o.fields_seen, 0u);
}

TEST(StateMachineCorruption, single_byte) {
    char const buf[] = "8";
    auto o = probe(buf, 1);
    EXPECT_FALSE(o.complete);
}

TEST(StateMachineCorruption, begin_string_truncated_no_soh) {
    char const buf[] = "8=FIX.4.2";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_FALSE(o.complete);
}

TEST(StateMachineCorruption, begin_string_only_then_soh_no_body_length) {
    char const buf[] = "8=FIX.4.2\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_FALSE(o.complete);
}

TEST(StateMachineCorruption, body_length_tag_without_value) {
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_FALSE(o.complete && o.valid);
}

TEST(StateMachineCorruption, body_length_non_numeric) {
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=abc\x01"
        "35=D\x01"
        "49=A\x01"
        "56=B\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    if (o.valid && o.complete)
        EXPECT_LE(o.fields_seen, 8u);
}

TEST(StateMachineCorruption, body_length_overflows_into_buffer_end) {
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=999999\x01"
        "35=D\x01"
        "49=A\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_FALSE(o.complete);
}

TEST(StateMachineCorruption, body_length_understated) {
    // BodyLength=5 but actual body is longer; CheckSum tag is not at the
    // location the header advertised.
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=5\x01"
        "35=D\x01"
        "49=ALICE\x01"
        "56=BOB\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_FALSE(o.valid && o.complete);
}

TEST(StateMachineCorruption, no_msg_type_after_body_length) {
    // Spec requires tag 35 immediately after tag 9.
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=12\x01"
        "49=A\x01"
        "56=B\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_LE(o.fields_seen, 8u);
}

TEST(StateMachineCorruption, soh_only_buffer) {
    char const buf[] = "\x01\x01\x01\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_FALSE(o.valid && o.complete);
}

TEST(StateMachineCorruption, consecutive_sohs_inside_message) {
    // Empty field value mid-message (two SOHs in a row).
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=00000018\x01"
        "35=D\x01"
        "49=\x01"
        "56=B\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    if (o.valid && o.complete)
        EXPECT_LE(o.fields_seen, 16u);
}

TEST(StateMachineCorruption, tag_without_equals_sign) {
    // Digits then non-'=' before SOH.
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=00000020\x01"
        "35=D\x01"
        "49ALICE\x01"
        "56=B\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_LE(o.fields_seen, 16u);
}

TEST(StateMachineCorruption, non_digit_in_tag_number) {
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=00000018\x01"
        "35=D\x01"
        "4a=A\x01"
        "56=B\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_LE(o.fields_seen, 16u);
}

TEST(StateMachineCorruption, raw_data_length_overshoots_buffer) {
    // RawDataLength=999 but only a few bytes follow.
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=00000035\x01"
        "35=n\x01"
        "49=A\x01"
        "56=B\x01"
        "95=999\x01"
        "96=hi\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_LE(o.fields_seen, 32u);
}

TEST(StateMachineCorruption, raw_data_length_negative_like) {
    // Negative-looking length ('-' rejected by atou, becomes 0).
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=00000030\x01"
        "35=n\x01"
        "49=A\x01"
        "56=B\x01"
        "95=-1\x01"
        "96=x\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_LE(o.fields_seen, 32u);
}

TEST(StateMachineCorruption, raw_data_length_huge_value) {
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=00000060\x01"
        "35=n\x01"
        "49=A\x01"
        "56=B\x01"
        "95=18446744073709551610\x01"
        "96=hi\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_LE(o.fields_seen, 32u);
}

TEST(StateMachineCorruption, two_back_to_back_headers) {
    std::string blob;
    blob +=
        "8=FIX.4.2\x01"
        "9=000010\x01"
        "35=A\x01"
        "10=000\x01";
    blob +=
        "8=FIX.4.2\x01"
        "9=000010\x01"
        "35=A\x01"
        "10=000\x01";
    int seen = 0;
    for (auto const& r : messages(blob.data(), blob.size())) {
        (void)r;
        if (++seen > 8) {
            FAIL() << "messages() did not terminate";
        }
    }
    EXPECT_LE(seen, 8);
}

TEST(StateMachineCorruption, header_8_fix_with_no_following_bytes) {
    char const buf[] = "8=FIX.";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_FALSE(o.complete);
}

TEST(StateMachineCorruption, messages_range_over_only_garbage) {
    char const buf[] = "completely_random_bytes_no_fix_anywhere_here\x01\x02\x03";
    int seen = 0;
    for (auto const& r : messages(buf, sizeof(buf) - 1)) {
        (void)r;
        if (++seen > 4) {
            FAIL() << "resync over garbage did not terminate";
        }
    }
    EXPECT_EQ(seen, 0);
}

TEST(StateMachineCorruption, body_length_with_leading_zeros_overflow) {
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=000500\x01"
        "35=D\x01"
        "49=A\x01"
        "56=B\x01"
        "10=000\x01";
    auto o = probe(buf, sizeof(buf) - 1);
    EXPECT_FALSE(o.complete);
}

TEST(StateMachineCorruption, indexed_path_on_corrupt_message_is_safe) {
    char const buf[] =
        "8=FIX.4.2\x01"
        "9=abc\x01"
        "35=D\x01"
        "49=A\x01"
        "10=000\x01";
    message_reader r(buf, buf + sizeof(buf) - 1);
    hffix::field_index_buffer<32> idx;
    auto im = hffix::build_field_index(r, idx);
    EXPECT_FALSE(im.field_count() > 32u);
}

TEST(SpanOverloads, writer_and_reader_round_trip_through_span) {
    std::array<char, 256> buf{};
    message_writer w{std::span<char>(buf)};
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "D");
    w.push_back_int(tag::OrderQty, 42);
    ASSERT_TRUE(w.push_back_trailer());

    std::span<char const> view{buf.data(), static_cast<std::size_t>(w.message_end() - buf.data())};
    message_reader r{view};
    ASSERT_TRUE(r.is_complete());
    ASSERT_TRUE(r.is_valid());

    message_reader::const_iterator i = r.begin();
    ASSERT_TRUE(r.find_with_hint(tag::OrderQty, i));
    EXPECT_EQ(i->value().as_int_unchecked<int>(), 42);
}

TEST(SpanOverloads, messages_and_for_each_message_accept_span) {
    char buf[512] = {};
    message_writer w(buf);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "0");
    ASSERT_TRUE(w.push_back_trailer());
    auto first_end = w.message_end();

    message_writer w2(first_end, buf + sizeof(buf));
    w2.push_back_header("FIXT.1.1");
    w2.push_back_string(tag::MsgType, "0");
    ASSERT_TRUE(w2.push_back_trailer());

    std::span<char const> view{buf, static_cast<std::size_t>(w2.message_end() - buf)};

    int count_from_range = 0;
    for (auto const& m : hffix::messages(view)) {
        EXPECT_TRUE(m.is_valid());
        ++count_from_range;
    }
    EXPECT_EQ(count_from_range, 2);

    int count_from_callback = 0;
    char const* tail = hffix::for_each_message(view, [&](message_reader const& m) noexcept {
        EXPECT_TRUE(m.is_valid());
        ++count_from_callback;
    });
    EXPECT_EQ(count_from_callback, 2);
    EXPECT_EQ(tail, view.data() + view.size());
}
