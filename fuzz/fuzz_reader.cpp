#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

#include "assert_override.hpp"

#include <hffix.hpp>
#include <hffix_fields.hpp>

namespace {

void fuzz_one(std::span<char const> wire) noexcept {
    hffix::message_reader r(wire);
    if (r.is_complete() && r.is_valid()) {
        for (auto it = r.begin(); it != r.end(); ++it) {
            hffix::field const& f = *it;
            auto const& v = f.value();

            int64_t i_signed = 0;
            uint64_t i_unsigned = 0;
            int64_t mantissa = 0, exponent = 0;
            (void)v.try_as_int(i_signed);
            (void)v.try_as_int(i_unsigned);
            (void)v.try_as_decimal(mantissa, exponent);

            int yy = 0, mm = 0, dd = 0;
            (void)v.as_date(yy, mm, dd);
            (void)v.as_monthyear(yy, mm);

            int h = 0, mi = 0, s = 0, ms = 0;
            (void)v.as_timeonly(h, mi, s, ms);

            (void)v.as_epoch_millis();
            (void)v.as_epoch_nanos();

            (void)v.size();
            (void)v.as_string_view();
        }

        (void)r.calculate_check_sum();
        (void)r.message_type();
        (void)r.check_sum();

        for (auto const& entry : r.group(hffix::tag::NoMDEntries, hffix::tag::MDEntryType)) {
            for (auto it = entry.begin(); it != entry.end(); ++it) {
                (void)it->tag();
            }
        }

        hffix::field_index_buffer<256> ibuf;
        auto idx = hffix::build_field_index(r, ibuf);
        if (idx.field_count() > 0) {
            std::size_t hint = 0;
            (void)idx.find_with_hint(hffix::tag::Price, hint);
            (void)idx.has(hffix::tag::MsgSeqNum);
        }
    }

    char const* tail = hffix::for_each_message(wire, [](hffix::message_reader const& m) noexcept {
        for (auto it = m.begin(); it != m.end(); ++it) {
            (void)it->tag();
        }
    });
    (void)tail;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size) {
    std::span<char const> wire(reinterpret_cast<char const*>(data), size);
    fuzz_one(wire);
    return 0;
}
