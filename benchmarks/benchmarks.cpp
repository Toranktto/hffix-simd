#include <benchmark/benchmark.h>

#include "assert_override.hpp"

#include <hffix.hpp>
#include <hffix_fields.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef HFFIX_BENCH_DATA_DIR
#define HFFIX_BENCH_DATA_DIR ""
#endif

namespace {

constexpr std::size_t kBufSize = 1 << 13;
constexpr std::size_t kBenchTimestampPool = 1024;
// Sized for the largest synthetic MD-incremental (~1.1k fields). The
// truncated/overflowed guards below SkipWithError if fixgen grows past this.
constexpr std::size_t kFieldIndexCapacity = 2048;

using SysTime = std::chrono::system_clock::time_point;

SysTime live_timestamp() noexcept {
    return std::chrono::system_clock::now();
}

std::size_t write_logon(char* buffer, std::size_t buffer_size, int seq, SysTime tsend) {
    hffix::message_writer w(buffer, buffer + buffer_size);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(hffix::tag::MsgType, "A");
    w.push_back_string(hffix::tag::SenderCompID, "AAAA");
    w.push_back_string(hffix::tag::TargetCompID, "BBBB");
    w.push_back_int(hffix::tag::MsgSeqNum, seq);
    w.push_back_timestamp(hffix::tag::SendingTime, tsend);
    w.push_back_int(hffix::tag::EncryptMethod, 0);
    w.push_back_int(hffix::tag::HeartBtInt, 10);
    (void)w.push_back_trailer();
    return static_cast<std::size_t>(w.message_end() - buffer);
}

std::size_t write_new_order(char* buffer, std::size_t buffer_size, int seq, SysTime tsend) {
    hffix::message_writer w(buffer, buffer + buffer_size);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(hffix::tag::MsgType, "D");
    w.push_back_string(hffix::tag::SenderCompID, "AAAA");
    w.push_back_string(hffix::tag::TargetCompID, "BBBB");
    w.push_back_int(hffix::tag::MsgSeqNum, seq);
    w.push_back_timestamp(hffix::tag::SendingTime, tsend);
    w.push_back_string(hffix::tag::ClOrdID, "A1");
    w.push_back_char(hffix::tag::HandlInst, '1');
    w.push_back_string(hffix::tag::Symbol, "OIH");
    w.push_back_char(hffix::tag::Side, '1');
    w.push_back_timestamp(hffix::tag::TransactTime, tsend);
    w.push_back_int(hffix::tag::OrderQty, 100);
    w.push_back_char(hffix::tag::OrdType, '2');
    w.push_back_decimal(hffix::tag::Price, 50001, -2);
    w.push_back_char(hffix::tag::TimeInForce, '1');
    (void)w.push_back_trailer();
    return static_cast<std::size_t>(w.message_end() - buffer);
}

// write_new_order with push_back_timestamp_epoch_nanos instead of the
// chrono time_point overload on SendingTime/TransactTime.
std::size_t write_new_order_epoch_nanos(char* buffer,
                                        std::size_t buffer_size,
                                        int seq,
                                        std::int64_t epoch_nanos) {
    hffix::message_writer w(buffer, buffer + buffer_size);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(hffix::tag::MsgType, "D");
    w.push_back_string(hffix::tag::SenderCompID, "AAAA");
    w.push_back_string(hffix::tag::TargetCompID, "BBBB");
    w.push_back_int(hffix::tag::MsgSeqNum, seq);
    w.push_back_timestamp_epoch_nanos(hffix::tag::SendingTime, epoch_nanos);
    w.push_back_string(hffix::tag::ClOrdID, "A1");
    w.push_back_char(hffix::tag::HandlInst, '1');
    w.push_back_string(hffix::tag::Symbol, "OIH");
    w.push_back_char(hffix::tag::Side, '1');
    w.push_back_timestamp_epoch_nanos(hffix::tag::TransactTime, epoch_nanos);
    w.push_back_int(hffix::tag::OrderQty, 100);
    w.push_back_char(hffix::tag::OrdType, '2');
    w.push_back_decimal(hffix::tag::Price, 50001, -2);
    w.push_back_char(hffix::tag::TimeInForce, '1');
    (void)w.push_back_trailer();
    return static_cast<std::size_t>(w.message_end() - buffer);
}

std::vector<char> load_file(std::filesystem::path const& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in)
        return {};
    auto size = in.tellg();
    if (size <= 0)
        return {};
    std::vector<char> buf(static_cast<std::size_t>(size));
    in.seekg(0);
    in.read(buf.data(), size);
    return buf;
}

struct Dataset {
    std::string name;
    std::vector<char> data;
};

std::vector<Dataset> const& load_datasets() {
    static std::vector<Dataset> const datasets = []() {
        std::vector<Dataset> out;
        std::filesystem::path dir{HFFIX_BENCH_DATA_DIR};
        if (dir.empty() || !std::filesystem::exists(dir))
            return out;
        // Sort for stable bench names across hosts.
        std::vector<std::filesystem::path> paths;
        for (auto const& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.path().extension() == ".fix")
                paths.push_back(entry.path());
        }
        std::sort(paths.begin(), paths.end());
        for (auto const& p : paths) {
            auto buf = load_file(p);
            if (buf.empty())
                continue;
            out.push_back({p.stem().string(), std::move(buf)});
        }
        return out;
    }();
    return datasets;
}

constexpr int kCommonTags[] = {
    hffix::tag::MsgType,
    hffix::tag::SenderCompID,
    hffix::tag::TargetCompID,
    hffix::tag::MsgSeqNum,
    hffix::tag::SendingTime,
};

// 10 present-in-MD + 5 absent; mixed worst/best case.
constexpr int kManyTags[] = {
    hffix::tag::MsgType,
    hffix::tag::SenderCompID,
    hffix::tag::TargetCompID,
    hffix::tag::MsgSeqNum,
    hffix::tag::SendingTime,
    hffix::tag::Symbol,
    hffix::tag::NoMDEntries,
    hffix::tag::MDEntryType,
    hffix::tag::MDEntryPx,
    hffix::tag::MDEntrySize,
    hffix::tag::ClOrdID,
    hffix::tag::OrderQty,
    hffix::tag::Price,
    hffix::tag::OrdType,
    hffix::tag::TimeInForce,
};

// 5 header + 5 MD body + 20 absent; iterator worst case.
constexpr int kWideTags[] = {
    hffix::tag::MsgType,      hffix::tag::SenderCompID, hffix::tag::TargetCompID,
    hffix::tag::MsgSeqNum,    hffix::tag::SendingTime,  hffix::tag::Symbol,
    hffix::tag::NoMDEntries,  hffix::tag::MDEntryType,  hffix::tag::MDEntryPx,
    hffix::tag::MDEntrySize,  hffix::tag::ClOrdID,      hffix::tag::OrderQty,
    hffix::tag::Price,        hffix::tag::OrdType,      hffix::tag::TimeInForce,
    hffix::tag::Account,      hffix::tag::ExpireTime,   hffix::tag::Side,
    hffix::tag::OrderID,      hffix::tag::ExecID,       hffix::tag::HandlInst,
    hffix::tag::TransactTime, hffix::tag::AvgPx,        hffix::tag::LastPx,
    hffix::tag::LastQty,      hffix::tag::SecurityID,   hffix::tag::CumQty,
    hffix::tag::LeavesQty,    hffix::tag::OrdStatus,    hffix::tag::ExecType,
};

// 5 session headers + 10 ExecReport body tags; ~55-65% per-tag hit against
// fixgen's default mix, not an all-present baseline.
constexpr int kWideTagsHighHit[] = {
    hffix::tag::MsgType,
    hffix::tag::SenderCompID,
    hffix::tag::TargetCompID,
    hffix::tag::MsgSeqNum,
    hffix::tag::SendingTime,
    hffix::tag::Symbol,
    hffix::tag::Side,
    hffix::tag::OrderID,
    hffix::tag::ExecID,
    hffix::tag::OrdStatus,
    hffix::tag::ExecType,
    hffix::tag::LeavesQty,
    hffix::tag::CumQty,
    hffix::tag::AvgPx,
    hffix::tag::LastPx,
};

// On-wire order vs reversed. find_with_hint carries hint across calls;
// reversed forces wrap.
constexpr int kSeqOrderTags[] = {
    hffix::tag::MsgType,
    hffix::tag::SenderCompID,
    hffix::tag::TargetCompID,
    hffix::tag::MsgSeqNum,
    hffix::tag::SendingTime,
    hffix::tag::Symbol,
    hffix::tag::NoMDEntries,
    hffix::tag::MDEntryType,
    hffix::tag::MDEntryPx,
    hffix::tag::MDEntrySize,
};
constexpr int kRandOrderTags[] = {
    hffix::tag::MDEntrySize,
    hffix::tag::MDEntryPx,
    hffix::tag::MDEntryType,
    hffix::tag::NoMDEntries,
    hffix::tag::Symbol,
    hffix::tag::SendingTime,
    hffix::tag::MsgSeqNum,
    hffix::tag::TargetCompID,
    hffix::tag::SenderCompID,
    hffix::tag::MsgType,
};

constexpr int kSingleMsgTags[] = {
    hffix::tag::MsgType,
    hffix::tag::SenderCompID,
    hffix::tag::TargetCompID,
    hffix::tag::MsgSeqNum,
    hffix::tag::SendingTime,
    hffix::tag::ClOrdID,
    hffix::tag::HandlInst,
    hffix::tag::Symbol,
    hffix::tag::Side,
    hffix::tag::OrderQty,
    hffix::tag::OrdType,
    hffix::tag::Price,
    hffix::tag::TimeInForce,
    hffix::tag::TransactTime,
    hffix::tag::Account,
};

void BM_WriteLogon(benchmark::State& state) {
    char buffer[kBufSize];
    auto tsend = live_timestamp();
    benchmark::DoNotOptimize(tsend);
    std::size_t total = 0;
    for (auto _ : state) {
        std::size_t n = write_logon(buffer, sizeof(buffer), 1, tsend);
        benchmark::DoNotOptimize(buffer);
        benchmark::ClobberMemory();
        total += n;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

void BM_WriteNewOrder(benchmark::State& state) {
    char buffer[kBufSize];
    auto tsend = live_timestamp();
    benchmark::DoNotOptimize(tsend);
    std::size_t total = 0;
    for (auto _ : state) {
        std::size_t n = write_new_order(buffer, sizeof(buffer), 1, tsend);
        benchmark::DoNotOptimize(buffer);
        benchmark::ClobberMemory();
        total += n;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

void BM_WriteNewOrder_EpochNanos(benchmark::State& state) {
    char buffer[kBufSize];
    auto epoch_nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(live_timestamp().time_since_epoch()).count();
    benchmark::DoNotOptimize(epoch_nanos);
    std::size_t total = 0;
    for (auto _ : state) {
        std::size_t n = write_new_order_epoch_nanos(buffer, sizeof(buffer), 1, epoch_nanos);
        benchmark::DoNotOptimize(buffer);
        benchmark::ClobberMemory();
        total += n;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

// Closure form via try_write_message: exercises the noexcept-overflow path
// and reports the same shape as BM_WriteNewOrder.
void BM_WriteNewOrder_Closure(benchmark::State& state) {
    char buffer[kBufSize];
    auto tsend = live_timestamp();
    benchmark::DoNotOptimize(tsend);
    std::size_t total = 0;
    for (auto _ : state) {
        char* end_out = nullptr;
        bool ok = hffix::try_write_message(
            buffer, buffer + sizeof(buffer), end_out, [&](hffix::message_writer& w) {
                w.push_back_header("FIXT.1.1");
                w.push_back_string(hffix::tag::MsgType, "D");
                w.push_back_string(hffix::tag::SenderCompID, "AAAA");
                w.push_back_string(hffix::tag::TargetCompID, "BBBB");
                w.push_back_int(hffix::tag::MsgSeqNum, 1);
                w.push_back_timestamp(hffix::tag::SendingTime, tsend);
                w.push_back_string(hffix::tag::ClOrdID, "A1");
                w.push_back_char(hffix::tag::HandlInst, '1');
                w.push_back_string(hffix::tag::Symbol, "OIH");
                w.push_back_char(hffix::tag::Side, '1');
                w.push_back_timestamp(hffix::tag::TransactTime, tsend);
                w.push_back_int(hffix::tag::OrderQty, 100);
                w.push_back_char(hffix::tag::OrdType, '2');
                w.push_back_decimal(hffix::tag::Price, 50001, -2);
                w.push_back_char(hffix::tag::TimeInForce, '1');
            });
        benchmark::DoNotOptimize(ok);
        benchmark::DoNotOptimize(buffer);
        benchmark::ClobberMemory();
        total += static_cast<std::size_t>(end_out - buffer);
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

void BM_ReadMessageScan(benchmark::State& state) {
    char buffer[kBufSize];
    auto tsend = live_timestamp();
    benchmark::DoNotOptimize(tsend);
    std::size_t len = write_new_order(buffer, sizeof(buffer), 1, tsend);
    std::size_t total = 0;
    for (auto _ : state) {
        hffix::message_reader r(buffer, buffer + len);
        if (r.is_complete() && r.is_valid()) {
            for (auto it = r.begin(); it != r.end(); ++it) {
                benchmark::DoNotOptimize(it->tag());
            }
        }
        total += len;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

void BM_ReadMessageFindFields(benchmark::State& state) {
    char buffer[kBufSize];
    auto tsend = live_timestamp();
    benchmark::DoNotOptimize(tsend);
    std::size_t len = write_new_order(buffer, sizeof(buffer), 1, tsend);
    std::size_t total = 0;
    for (auto _ : state) {
        hffix::message_reader r(buffer, buffer + len);
        if (r.is_complete() && r.is_valid()) {
            auto it = r.begin();
            if (r.find_with_hint(hffix::tag::ClOrdID, it)) {
                auto v = it->value();
                benchmark::DoNotOptimize(v);
            }
            if (r.find_with_hint(hffix::tag::Symbol, it)) {
                auto v = it->value();
                benchmark::DoNotOptimize(v);
            }
            if (r.find_with_hint(hffix::tag::OrderQty, it)) {
                auto v = it->value().as_int_unchecked<int>();
                benchmark::DoNotOptimize(v);
            }
            if (r.find_with_hint(hffix::tag::Price, it)) {
                auto v = it->value();
                benchmark::DoNotOptimize(v);
            }
        }
        total += len;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

void BM_RoundTrip(benchmark::State& state) {
    char buffer[kBufSize];
    auto tsend = live_timestamp();
    benchmark::DoNotOptimize(tsend);
    std::size_t total = 0;
    for (auto _ : state) {
        std::size_t len = write_new_order(buffer, sizeof(buffer), 1, tsend);
        hffix::message_reader r(buffer, buffer + len);
        for (auto it = r.begin(); it != r.end(); ++it) {
            auto v = it->value();
            benchmark::DoNotOptimize(v);
        }
        total += len;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

std::vector<std::string> make_timestamp_pool_millis() {
    std::vector<std::string> out;
    out.reserve(kBenchTimestampPool);
    char buf[32];
    for (std::size_t i = 0; i < kBenchTimestampPool; ++i) {
        int year = 2000 + static_cast<int>((i * 13) % 99);
        int month = 1 + static_cast<int>((i * 7) % 12);
        int day = 1 + static_cast<int>((i * 11) % 28);
        int hour = static_cast<int>((i * 5) % 24);
        int minute = static_cast<int>((i * 17) % 60);
        int second = static_cast<int>((i * 31) % 60);
        int millis = static_cast<int>((i * 137) % 1000);
        std::snprintf(buf,
                      sizeof(buf),
                      "%04d%02d%02d-%02d:%02d:%02d.%03d",
                      year,
                      month,
                      day,
                      hour,
                      minute,
                      second,
                      millis);
        out.emplace_back(buf);
    }
    return out;
}

std::vector<std::string> make_timestamp_pool_nanos() {
    std::vector<std::string> out;
    out.reserve(kBenchTimestampPool);
    char buf[40];
    for (std::size_t i = 0; i < kBenchTimestampPool; ++i) {
        int year = 2000 + static_cast<int>((i * 13) % 99);
        int month = 1 + static_cast<int>((i * 7) % 12);
        int day = 1 + static_cast<int>((i * 11) % 28);
        int hour = static_cast<int>((i * 5) % 24);
        int minute = static_cast<int>((i * 17) % 60);
        int second = static_cast<int>((i * 31) % 60);
        std::uint64_t nanos = (static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ULL) % 1000000000ULL;
        std::snprintf(buf,
                      sizeof(buf),
                      "%04d%02d%02d-%02d:%02d:%02d.%09llu",
                      year,
                      month,
                      day,
                      hour,
                      minute,
                      second,
                      static_cast<unsigned long long>(nanos));
        out.emplace_back(buf);
    }
    return out;
}

void BM_ParseTimestampMillis(benchmark::State& state) {
    static auto const pool = make_timestamp_pool_millis();
    using TimePoint = std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds>;
    std::size_t i = 0;
    std::size_t total = 0;
    for (auto _ : state) {
        auto const& s = pool[i++ % pool.size()];
        TimePoint tp;
        bool ok = hffix::details::atotimepoint(s.data(), s.data() + s.size(), tp);
        benchmark::DoNotOptimize(ok);
        benchmark::DoNotOptimize(tp);
        total += s.size();
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

void BM_ParseTimestampNanos(benchmark::State& state) {
    static auto const pool = make_timestamp_pool_nanos();
    using TimePoint = std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds>;
    std::size_t i = 0;
    std::size_t total = 0;
    for (auto _ : state) {
        auto const& s = pool[i++ % pool.size()];
        TimePoint tp;
        bool ok = hffix::details::atotimepoint_nano(s.data(), s.data() + s.size(), tp);
        benchmark::DoNotOptimize(ok);
        benchmark::DoNotOptimize(tp);
        total += s.size();
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

void BM_FindManyFields_Iterator(benchmark::State& state) {
    char buffer[kBufSize];
    auto tsend = live_timestamp();
    benchmark::DoNotOptimize(tsend);
    std::size_t len = write_new_order(buffer, sizeof(buffer), 1, tsend);
    int n_lookups = static_cast<int>(state.range(0));
    std::size_t total = 0;
    for (auto _ : state) {
        hffix::message_reader r(buffer, buffer + len);
        if (!r.is_valid())
            continue;
        for (int i = 0; i < n_lookups; ++i) {
            auto it = r.begin();
            if (r.find_with_hint(kSingleMsgTags[i], it)) {
                auto v = it->value();
                benchmark::DoNotOptimize(v);
            }
        }
        total += len;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

void BM_FindManyFields_Indexed(benchmark::State& state) {
    char buffer[kBufSize];
    auto tsend = live_timestamp();
    benchmark::DoNotOptimize(tsend);
    std::size_t len = write_new_order(buffer, sizeof(buffer), 1, tsend);
    hffix::field_index_buffer<32> idx_buffer;
    int n_lookups = static_cast<int>(state.range(0));
    std::size_t total = 0;
    for (auto _ : state) {
        hffix::message_reader r(buffer, buffer + len);
        if (!r.is_valid())
            continue;
        auto idx = hffix::build_field_index(r, idx_buffer);
        for (int i = 0; i < n_lookups; ++i) {
            std::size_t h = 0;
            auto v = idx.find_with_hint(kSingleMsgTags[i], h);
            benchmark::DoNotOptimize(v);
        }
        total += len;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total));
}

void BM_Parse_ScanMessages(benchmark::State& state, Dataset const* ds) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    std::size_t total_bytes = 0;
    std::size_t total_messages = 0;
    for (auto _ : state) {
        std::size_t messages = 0;
        hffix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (r.is_valid()) {
                ++messages;
                benchmark::DoNotOptimize(messages);
            }
        }
        total_bytes += ds->data.size();
        total_messages += messages;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_messages));
}

void BM_Parse_IterAllFields(benchmark::State& state, Dataset const* ds) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    std::size_t total_bytes = 0;
    std::size_t total_fields = 0;
    for (auto _ : state) {
        std::size_t fields = 0;
        hffix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            for (auto it = r.begin(); it != r.end(); ++it) {
                ++fields;
                benchmark::DoNotOptimize(it->tag());
            }
        }
        total_bytes += ds->data.size();
        total_fields += fields;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_fields));
}

template <int const* Tags, std::size_t N>
void BM_Parse_FindTags(benchmark::State& state, Dataset const* ds) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    std::size_t total_bytes = 0;
    std::size_t total_messages = 0;
    for (auto _ : state) {
        std::size_t messages = 0;
        hffix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            ++messages;
            auto it = r.begin();
            for (std::size_t k = 0; k < N; ++k) {
                if (r.find_with_hint(Tags[k], it)) {
                    auto v = it->value();
                    benchmark::DoNotOptimize(v);
                }
            }
        }
        total_bytes += ds->data.size();
        total_messages += messages;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_messages));
}

// Hint hoisted: lookup order matters. The non-Hint variant resets per call.
template <int const* Tags, std::size_t N>
void BM_Parse_FindTagsIndexed_Hint(benchmark::State& state, Dataset const* ds) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    hffix::field_index_buffer<kFieldIndexCapacity> idx_buffer;
    std::size_t total_bytes = 0;
    std::size_t total_messages = 0;
    for (auto _ : state) {
        std::size_t messages = 0;
        hffix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            ++messages;
            auto idx = hffix::build_field_index(r, idx_buffer);
            std::size_t h = 0;
            for (std::size_t k = 0; k < N; ++k) {
                auto v = idx.find_with_hint(Tags[k], h);
                benchmark::DoNotOptimize(v);
            }
            if (!idx.authoritative()) [[unlikely]] {
                state.SkipWithError("index truncated or overflowed; bump kFieldIndexCapacity");
                return;
            }
        }
        total_bytes += ds->data.size();
        total_messages += messages;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_messages));
}

// Counters include the clock::now() probe (~20-30 ns on M4); same overhead
// in every variant, so relative deltas are comparable, absolute p* are not.
template <int const* Tags, std::size_t N, bool UseIndex>
void BM_Parse_TailLatency(benchmark::State& state, Dataset const* ds) {
    using clk = std::chrono::steady_clock;
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    hffix::field_index_buffer<kFieldIndexCapacity> idx_buffer;
    std::vector<std::uint64_t> samples;
    samples.reserve(1u << 20);
    for (auto _ : state) {
        state.PauseTiming();
        samples.clear();
        state.ResumeTiming();
        hffix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            auto t0 = clk::now();
            if constexpr (UseIndex) {
                auto idx = hffix::build_field_index(r, idx_buffer);
                std::size_t h = 0;
                for (std::size_t k = 0; k < N; ++k) {
                    auto v = idx.find_with_hint(Tags[k], h);
                    benchmark::DoNotOptimize(v);
                }
            } else {
                auto it = r.begin();
                for (std::size_t k = 0; k < N; ++k) {
                    if (r.find_with_hint(Tags[k], it)) {
                        auto v = it->value();
                        benchmark::DoNotOptimize(v);
                    }
                }
            }
            auto t1 = clk::now();
            samples.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
        }
    }
    state.PauseTiming();
    if (samples.empty()) {
        state.SkipWithError("no messages measured");
        return;
    }
    std::sort(samples.begin(), samples.end());
    auto pct = [&](double q) -> double {
        std::size_t i = static_cast<std::size_t>(q * (samples.size() - 1));
        return static_cast<double>(samples[i]);
    };
    state.counters["p95_ns"] = pct(0.95);
    state.counters["p99_ns"] = pct(0.99);
    state.counters["p999_ns"] = pct(0.999);
    state.counters["max_ns"] = static_cast<double>(samples.back());
    state.counters["samples"] = static_cast<double>(samples.size());
}

template <int const* Tags, std::size_t N>
void BM_Parse_FindTagsIndexed(benchmark::State& state, Dataset const* ds) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    hffix::field_index_buffer<kFieldIndexCapacity> idx_buffer;
    std::size_t total_bytes = 0;
    std::size_t total_messages = 0;
    for (auto _ : state) {
        std::size_t messages = 0;
        hffix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            ++messages;
            auto idx = hffix::build_field_index(r, idx_buffer);
            for (std::size_t k = 0; k < N; ++k) {
                std::size_t h = 0;
                auto v = idx.find_with_hint(Tags[k], h);
                benchmark::DoNotOptimize(v);
            }
            if (!idx.authoritative()) [[unlikely]] {
                state.SkipWithError("index truncated or overflowed; bump kFieldIndexCapacity");
                return;
            }
        }
        total_bytes += ds->data.size();
        total_messages += messages;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_messages));
}

void BM_Parse_BuildFieldIndex(benchmark::State& state, Dataset const* ds) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    hffix::field_index_buffer<kFieldIndexCapacity> idx_buffer;
    std::size_t total_bytes = 0;
    std::size_t total_messages = 0;
    for (auto _ : state) {
        std::size_t messages = 0;
        hffix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            ++messages;
            auto idx = hffix::build_field_index(r, idx_buffer);
            benchmark::DoNotOptimize(idx);
            if (!idx.authoritative()) [[unlikely]] {
                state.SkipWithError("index truncated or overflowed; bump kFieldIndexCapacity");
                return;
            }
        }
        total_bytes += ds->data.size();
        total_messages += messages;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_messages));
}

void BM_Parse_FindN_Iter(benchmark::State& state, Dataset const* ds, int n) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    std::size_t total_bytes = 0;
    std::size_t total_messages = 0;
    for (auto _ : state) {
        std::size_t messages = 0;
        hffix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            ++messages;
            auto it = r.begin();
            for (int i = 0; i < n; ++i) {
                if (r.find_with_hint(kWideTags[i], it)) {
                    auto v = it->value();
                    benchmark::DoNotOptimize(v);
                }
            }
        }
        total_bytes += ds->data.size();
        total_messages += messages;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_messages));
}

void BM_Parse_FindN_Indexed(benchmark::State& state, Dataset const* ds, int n) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    hffix::field_index_buffer<kFieldIndexCapacity> idx_buffer;
    std::size_t total_bytes = 0;
    std::size_t total_messages = 0;
    for (auto _ : state) {
        std::size_t messages = 0;
        hffix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            ++messages;
            auto idx = hffix::build_field_index(r, idx_buffer);
            for (int i = 0; i < n; ++i) {
                std::size_t h = 0;
                auto v = idx.find_with_hint(kWideTags[i], h);
                benchmark::DoNotOptimize(v);
            }
            if (!idx.authoritative()) [[unlikely]] {
                state.SkipWithError("index truncated or overflowed; bump kFieldIndexCapacity");
                return;
            }
        }
        total_bytes += ds->data.size();
        total_messages += messages;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_messages));
}

// reader.group<>() sugar vs hand-rolled delimiter scan. Same lookups
// (MDEntryPx, MDEntrySize per entry). Throughput must match.
void BM_Parse_GroupAccess_Sugared(benchmark::State& state, Dataset const* ds) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    std::size_t total_bytes = 0;
    std::size_t total_messages = 0;
    for (auto _ : state) {
        std::size_t messages = 0;
        hffix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            ++messages;
            for (auto const& entry : r.group<hffix::tag::NoMDEntries>()) {
                auto eit = entry.begin();
                if (entry.find_with_hint(hffix::tag::MDEntryPx, eit)) {
                    auto v = eit->value();
                    benchmark::DoNotOptimize(v);
                }
                eit = entry.begin();
                if (entry.find_with_hint(hffix::tag::MDEntrySize, eit)) {
                    auto v = eit->value();
                    benchmark::DoNotOptimize(v);
                }
            }
        }
        total_bytes += ds->data.size();
        total_messages += messages;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_messages));
}

void BM_Parse_GroupAccess_Manual(benchmark::State& state, Dataset const* ds) {
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
    constexpr int kDelimiter = hffix::tag::MDEntryType;
    std::size_t total_bytes = 0;
    std::size_t total_messages = 0;
    for (auto _ : state) {
        std::size_t messages = 0;
        hffix::message_reader r(begin, end);
        for (; r.is_complete(); r = r.next_message_reader()) {
            if (!r.is_valid())
                continue;
            ++messages;
            auto it = r.begin();
            auto const end_it = r.end();
            if (!r.find_with_hint(hffix::tag::NoMDEntries, it))
                continue;
            std::size_t count = 0;
            if (!it->value().try_as_int<std::size_t>(count))
                continue;
            ++it;
            while (it != end_it && it->tag() != kDelimiter)
                ++it;
            std::size_t seen = 0;
            while (seen < count && it != end_it) {
                auto entry_begin = it;
                ++it;
                while (it != end_it && it->tag() != kDelimiter)
                    ++it;
                auto entry_end = it;
                auto eit = entry_begin;
                if (hffix::find_with_hint(
                        entry_begin, entry_end, hffix::tag_equal(hffix::tag::MDEntryPx), eit)) {
                    auto v = eit->value();
                    benchmark::DoNotOptimize(v);
                }
                eit = entry_begin;
                if (hffix::find_with_hint(
                        entry_begin, entry_end, hffix::tag_equal(hffix::tag::MDEntrySize), eit)) {
                    auto v = eit->value();
                    benchmark::DoNotOptimize(v);
                }
                ++seen;
            }
        }
        total_bytes += ds->data.size();
        total_messages += messages;
    }
    state.SetBytesProcessed(static_cast<int64_t>(total_bytes));
    state.SetItemsProcessed(static_cast<int64_t>(total_messages));
}

constexpr int kAmortizeN[] = {3, 5, 7, 8, 10, 13, 15, 20, 30};

void register_benches() {
    benchmark::RegisterBenchmark("BM_WriteLogon", BM_WriteLogon);
    benchmark::RegisterBenchmark("BM_WriteNewOrder", BM_WriteNewOrder);
    benchmark::RegisterBenchmark("BM_WriteNewOrder_EpochNanos", BM_WriteNewOrder_EpochNanos);
    benchmark::RegisterBenchmark("BM_WriteNewOrder_Closure", BM_WriteNewOrder_Closure);
    benchmark::RegisterBenchmark("BM_ReadMessageScan", BM_ReadMessageScan);
    benchmark::RegisterBenchmark("BM_ReadMessageFindFields", BM_ReadMessageFindFields);
    benchmark::RegisterBenchmark("BM_RoundTrip", BM_RoundTrip);
    benchmark::RegisterBenchmark("BM_ParseTimestampMillis", BM_ParseTimestampMillis);
    benchmark::RegisterBenchmark("BM_ParseTimestampNanos", BM_ParseTimestampNanos);

    auto* iter_b =
        benchmark::RegisterBenchmark("BM_FindManyFields_Iterator", BM_FindManyFields_Iterator);
    auto* idx_b =
        benchmark::RegisterBenchmark("BM_FindManyFields_Indexed", BM_FindManyFields_Indexed);
    for (int n : {3, 5, 8, 13, 15}) {
        iter_b->Arg(n);
        idx_b->Arg(n);
    }

    auto const& datasets = load_datasets();
    if (datasets.empty()) {
        benchmark::RegisterBenchmark("BM_Parse/<no-dataset>", [](benchmark::State& s) {
            s.SkipWithError("no dataset; run cmake --build <build> --target bench_data");
        });
        return;
    }
    for (auto const& ds : datasets) {
        std::string const& name = ds.name;
        benchmark::RegisterBenchmark(
            ("BM_Parse_ScanMessages/" + name).c_str(), BM_Parse_ScanMessages, &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_IterAllFields/" + name).c_str(), BM_Parse_IterAllFields, &ds);
        benchmark::RegisterBenchmark(("BM_Parse_FindCommonFields/" + name).c_str(),
                                     BM_Parse_FindTags<kCommonTags, std::size(kCommonTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(("BM_Parse_FindManyFields_Iter/" + name).c_str(),
                                     BM_Parse_FindTags<kManyTags, std::size(kManyTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(("BM_Parse_FindLotsFields_Iter/" + name).c_str(),
                                     BM_Parse_FindTags<kWideTags, std::size(kWideTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(("BM_Parse_FindCommonFields_Indexed/" + name).c_str(),
                                     BM_Parse_FindTagsIndexed<kCommonTags, std::size(kCommonTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(("BM_Parse_FindManyFields_Indexed/" + name).c_str(),
                                     BM_Parse_FindTagsIndexed<kManyTags, std::size(kManyTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(("BM_Parse_FindLotsFields_Indexed/" + name).c_str(),
                                     BM_Parse_FindTagsIndexed<kWideTags, std::size(kWideTags)>,
                                     &ds);
        // HighHit: 5 always-present headers + 10 partial ExecReport body tags
        // (~55-65% per-tag hit rate on fixgen's mix); not an all-present baseline.
        benchmark::RegisterBenchmark(("BM_Parse_FindManyFields_Iter_HighHit/" + name).c_str(),
                                     BM_Parse_FindTags<kWideTagsHighHit, std::size(kWideTagsHighHit)>,
                                     &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_FindManyFields_Indexed_HighHit/" + name).c_str(),
            BM_Parse_FindTagsIndexed<kWideTagsHighHit, std::size(kWideTagsHighHit)>,
            &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_BuildFieldIndex/" + name).c_str(), BM_Parse_BuildFieldIndex, &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_GroupAccess_Sugared/" + name).c_str(), BM_Parse_GroupAccess_Sugared, &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_GroupAccess_Manual/" + name).c_str(), BM_Parse_GroupAccess_Manual, &ds);

        benchmark::RegisterBenchmark(("BM_Parse_Sequential_Iter/" + name).c_str(),
                                     BM_Parse_FindTags<kSeqOrderTags, std::size(kSeqOrderTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(("BM_Parse_Random_Iter/" + name).c_str(),
                                     BM_Parse_FindTags<kRandOrderTags, std::size(kRandOrderTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_Sequential_Indexed/" + name).c_str(),
            BM_Parse_FindTagsIndexed_Hint<kSeqOrderTags, std::size(kSeqOrderTags)>,
            &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_Random_Indexed/" + name).c_str(),
            BM_Parse_FindTagsIndexed_Hint<kRandOrderTags, std::size(kRandOrderTags)>,
            &ds);

        benchmark::RegisterBenchmark(
            ("BM_Parse_TailLatency_Sequential_Iter/" + name).c_str(),
            BM_Parse_TailLatency<kSeqOrderTags, std::size(kSeqOrderTags), false>,
            &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_TailLatency_Random_Iter/" + name).c_str(),
            BM_Parse_TailLatency<kRandOrderTags, std::size(kRandOrderTags), false>,
            &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_TailLatency_Sequential_Indexed/" + name).c_str(),
            BM_Parse_TailLatency<kSeqOrderTags, std::size(kSeqOrderTags), true>,
            &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_TailLatency_Random_Indexed/" + name).c_str(),
            BM_Parse_TailLatency<kRandOrderTags, std::size(kRandOrderTags), true>,
            &ds);

        for (int n : kAmortizeN) {
            std::string n_str = std::to_string(n);
            benchmark::RegisterBenchmark(
                ("BM_Parse_FindN_Iter/" + n_str + "/" + name).c_str(), BM_Parse_FindN_Iter, &ds, n);
            benchmark::RegisterBenchmark(("BM_Parse_FindN_Indexed/" + n_str + "/" + name).c_str(),
                                         BM_Parse_FindN_Indexed,
                                         &ds,
                                         n);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    register_benches();
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
        return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
