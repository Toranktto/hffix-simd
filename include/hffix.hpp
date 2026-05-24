/*******************************************************************************************
Copyright 2011, T3 IP, LLC. All rights reserved.
Copyright 2026, Łukasz Derlatka (modifications). All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are
permitted provided that the following conditions are met:

   1. Redistributions of source code must retain the above copyright notice, this list of
      conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright notice, this list
      of conditions and the following disclaimer in the documentation and/or other materials
      provided with the distribution.

THIS SOFTWARE IS PROVIDED BY T3 IP, LLC "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL T3 IP, LLC OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The views and conclusions contained in the software and documentation are those of the
authors and should not be interpreted as representing official policies, either expressed
or implied, of T3 IP, LLC.
*******************************************************************************************/

/**
 * \file
 * \brief The High Frequency FIX Parser Library.
 */

#pragma once

#include <algorithm>    // for is_tag_a_data_length
#include <atomic>       // for assert_failure_counter
#include <bit>          // for std::endian
#include <cstdint>      // for std::uint8_t
#include <cstring>      // for memcpy
#include <iostream>     // for operator<<()
#include <limits>       // for numeric_limits<>::is_signed
#include <optional>     // for as_epoch_* return type
#include <span>         // for basic_indexed_message storage view
#include <string_view>  // for push_back_string()
// Angle brackets so downstream can override via -I order.
#include <chrono>
#include <string>
#include <hffix_fields.hpp>

#if defined(__GNUC__) || defined(__clang__)
#define HFFIX_ALWAYS_INLINE inline __attribute__((always_inline))
#define HFFIX_PREFETCH(p) __builtin_prefetch((p))
#define HFFIX_HOT __attribute__((hot))
#elif defined(_MSC_VER)
#define HFFIX_ALWAYS_INLINE __forceinline
#define HFFIX_PREFETCH(p) ((void)0)
#define HFFIX_HOT
#else
#define HFFIX_ALWAYS_INLINE inline
#define HFFIX_PREFETCH(p) ((void)0)
#define HFFIX_HOT
#endif

#ifndef HFFIX_ASSERT
#ifdef NDEBUG
#define HFFIX_ASSERT(cond, msg)                                                                 \
    do {                                                                                        \
        if (!(cond)) [[unlikely]]                                                               \
            ::hffix::details::assert_failure_counter().fetch_add(1, std::memory_order_relaxed); \
    } while (0)
#elif defined(__GNUC__) || defined(__clang__)
#define HFFIX_ASSERT(cond, msg)   \
    do {                          \
        if (!(cond)) [[unlikely]] \
            __builtin_trap();     \
    } while (0)
#elif defined(_MSC_VER)
#define HFFIX_ASSERT(cond, msg)   \
    do {                          \
        if (!(cond)) [[unlikely]] \
            __debugbreak();       \
    } while (0)
#else
#include <cstdlib>
#define HFFIX_ASSERT(cond, msg)   \
    do {                          \
        if (!(cond)) [[unlikely]] \
            std::abort();         \
    } while (0)
#endif
#endif

/**
\brief Namespace for all types and functions of High Frequency FIX Parser.
*/
namespace hffix {

namespace details {
[[nodiscard]] inline std::atomic<std::uint64_t>& assert_failure_counter() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}
}  // namespace details

/** \brief Monotonic count of `HFFIX_ASSERT` failures. */
[[nodiscard]] inline std::uint64_t assert_failure_count() noexcept {
    return details::assert_failure_counter().load(std::memory_order_relaxed);
}

inline void reset_assert_failure_count() noexcept {
    details::assert_failure_counter().store(0, std::memory_order_relaxed);
}

template <std::size_t N>
struct field_index_buffer {
    static_assert(N > 0 && N <= 16384, "field_index_buffer<N>: N must be in (0, 16384]");

    alignas(32) int tags[N];
    alignas(32) std::uint64_t pos_len[N];  // (pos << 32) | len
    static constexpr std::size_t capacity = N;
};

/* @cond EXCLUDE */

namespace details {

HFFIX_ALWAYS_INLINE char const* find_soh(char const* begin, char const* end) {
    while (begin < end && *begin != '\x01') {
        ++begin;
    }
    return begin;
}

HFFIX_ALWAYS_INLINE std::size_t find_tag_in_index(int const* tags, std::size_t n, int tag) {
    for (std::size_t i = 0; i < n; ++i) {
        if (tags[i] == tag)
            return i;
    }
    return n;
}

HFFIX_ALWAYS_INLINE std::uint8_t checksum_bytes(char const* begin, char const* end) {
    std::uint8_t sum = 0;
    while (begin != end)
        sum = static_cast<std::uint8_t>(sum + std::uint8_t(*begin++));
    return sum;
}

template <std::size_t N>
std::ptrdiff_t len(char const (&)[N]) {
    return std::ptrdiff_t(N - 1);
}

static_assert(std::endian::native == std::endian::little,
              "hffix SWAR digit parsers assume little-endian byte order.");

HFFIX_ALWAYS_INLINE std::uint32_t parse_two_digits(char const* p) {
    return std::uint32_t(p[0] - '0') * 10 + std::uint32_t(p[1] - '0');
}

HFFIX_ALWAYS_INLINE std::uint32_t parse_four_digits(char const* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    v -= 0x30303030U;
    v = (v & 0x000f000fU) * 10 + ((v & 0x0f000f00U) >> 8);
    return (v & 0x000000ffU) * 100 + ((v & 0x00ff0000U) >> 16);
}

HFFIX_ALWAYS_INLINE std::uint32_t parse_eight_digits(char const* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    v -= 0x3030303030303030ULL;
    v = (v & 0x000f000f000f000fULL) * 10 + ((v & 0x0f000f000f000f00ULL) >> 8);
    v = (v & 0x000000ff000000ffULL) * 100 + ((v & 0x00ff000000ff0000ULL) >> 16);
    v = (v & 0x000000000000ffffULL) * 10000 + (v >> 32);
    return static_cast<std::uint32_t>(v);
}

template <typename Int_type>
Int_type atoi(char const* begin, char const* end) {
    using U = typename std::make_unsigned<Int_type>::type;
    U uval = 0;
    bool isnegative = false;

    if (begin < end && *begin == '-') {
        isnegative = true;
        ++begin;
    }

    for (; begin < end; ++begin) {
        uval = uval * 10u + static_cast<U>(static_cast<unsigned char>(*begin) - '0');
    }

    return isnegative ? static_cast<Int_type>(U{0} - uval) : static_cast<Int_type>(uval);
}

template <typename Uint_type>
inline Uint_type atou(char const* begin, char const* end) {
    Uint_type val(0);

    for (; begin < end; ++begin) {
        val *= 10u;
        val += (Uint_type)(*begin - '0');
    }

    return val;
}

template <typename Int_type>
void atod(char const* begin, char const* end, Int_type& mantissa, Int_type& exponent) {
    using U = std::make_unsigned_t<Int_type>;
    U m = 0;
    Int_type exponent_ = 0;
    bool isdecimal(false);
    bool isnegative(false);

    if (begin < end && *begin == '-') {
        isnegative = true;
        ++begin;
    }

    for (; begin < end; ++begin) {
        if (*begin == '.') {
            isdecimal = true;
        } else {
            m = m * 10u + static_cast<U>(static_cast<unsigned char>(*begin) - '0');
            if (isdecimal)
                --exponent_;
        }
    }

    mantissa = isnegative ? static_cast<Int_type>(U{0} - m) : static_cast<Int_type>(m);
    exponent = exponent_;
}

template <typename Int_type>
[[nodiscard]] inline bool try_atoi(char const* begin, char const* end, Int_type& out) noexcept {
    static_assert(std::numeric_limits<Int_type>::is_signed,
                  "try_atoi requires a signed Int_type; use try_atou for unsigned.");
    if (begin == end)
        return false;
    bool neg = false;
    if (*begin == '-') {
        neg = true;
        ++begin;
        if (begin == end)
            return false;
    }
    using U = std::make_unsigned_t<Int_type>;
    constexpr U max_pos = static_cast<U>(std::numeric_limits<Int_type>::max());
    U const limit = neg ? static_cast<U>(max_pos + U{1}) : max_pos;
    U val = 0;
    for (; begin < end; ++begin) {
        auto const c = static_cast<unsigned char>(*begin);
        if (c < '0' || c > '9') [[unlikely]]
            return false;
        U const d = static_cast<U>(c - '0');
        if (val > (limit - d) / 10u) [[unlikely]]
            return false;
        val = val * 10u + d;
    }
    out = neg ? static_cast<Int_type>(U{0} - val) : static_cast<Int_type>(val);
    return true;
}

template <typename Uint_type>
[[nodiscard]] inline bool try_atou(char const* begin, char const* end, Uint_type& out) noexcept {
    static_assert(!std::numeric_limits<Uint_type>::is_signed,
                  "try_atou requires an unsigned Uint_type; use try_atoi for signed.");
    if (begin == end)
        return false;
    constexpr Uint_type max = std::numeric_limits<Uint_type>::max();
    Uint_type val = 0;
    for (; begin < end; ++begin) {
        auto const c = static_cast<unsigned char>(*begin);
        if (c < '0' || c > '9') [[unlikely]]
            return false;
        Uint_type const d = static_cast<Uint_type>(c - '0');
        if (val > (max - d) / 10u) [[unlikely]]
            return false;
        val = val * 10u + d;
    }
    out = val;
    return true;
}

template <typename Int_type>
[[nodiscard]] inline bool try_atod(char const* begin,
                                   char const* end,
                                   Int_type& mantissa,
                                   Int_type& exponent) noexcept {
    static_assert(std::numeric_limits<Int_type>::is_signed,
                  "try_atod requires a signed Int_type for the mantissa.");
    if (begin == end)
        return false;
    bool neg = false;
    if (*begin == '-') {
        neg = true;
        ++begin;
        if (begin == end)
            return false;
    }
    using U = std::make_unsigned_t<Int_type>;
    constexpr U max_pos = static_cast<U>(std::numeric_limits<Int_type>::max());
    U const limit = neg ? static_cast<U>(max_pos + U{1}) : max_pos;
    U m = 0;
    Int_type e = 0;
    bool seen_dot = false;
    bool seen_digit = false;
    for (; begin < end; ++begin) {
        char const c = *begin;
        if (c == '.') {
            if (seen_dot) [[unlikely]]
                return false;
            seen_dot = true;
            continue;
        }
        auto const uc = static_cast<unsigned char>(c);
        if (uc < '0' || uc > '9') [[unlikely]]
            return false;
        seen_digit = true;
        U const d = static_cast<U>(uc - '0');
        if (m > (limit - d) / 10u) [[unlikely]]
            return false;
        m = m * 10u + d;
        if (seen_dot)
            --e;
    }
    if (!seen_digit) [[unlikely]]
        return false;
    mantissa = neg ? static_cast<Int_type>(U{0} - m) : static_cast<Int_type>(m);
    exponent = e;
    return true;
}

// Max ascii chars to print Int_type (digits + sign).
template <class Int_type>
inline constexpr std::ptrdiff_t max_ascii_chars = std::numeric_limits<Int_type>::digits10 + 2;

template <typename Uint_type>
HFFIX_ALWAYS_INLINE char* utoa_unchecked(Uint_type number, char* buffer) noexcept {
    char* b = buffer;
    do {
        *b++ = static_cast<char>('0' + (number % 10));
        number /= 10;
    } while (number);
    std::reverse(buffer, b);
    return b;
}

template <typename Int_type>
HFFIX_ALWAYS_INLINE char* itoa_unchecked(Int_type number, char* buffer) noexcept {
    using U = std::make_unsigned_t<Int_type>;
    bool const isnegative = number < 0;
    U n = isnegative ? U{0} - static_cast<U>(number) : static_cast<U>(number);
    char* b = buffer;
    do {
        *b++ = static_cast<char>('0' + (n % 10));
        n /= 10;
    } while (n);
    if (isnegative)
        *b++ = '-';
    std::reverse(buffer, b);
    return b;
}

template <typename Int_type>
HFFIX_ALWAYS_INLINE char* dtoa_unchecked(Int_type mantissa, Int_type exponent, char* buffer) noexcept {
    using U = std::make_unsigned_t<Int_type>;
    bool const isnegative = mantissa < 0;
    U m = isnegative ? U{0} - static_cast<U>(mantissa) : static_cast<U>(mantissa);
    char* b = buffer;
    do {
        *b++ = static_cast<char>('0' + (m % 10));
        m /= 10;
        if (++exponent == 0)
            *b++ = '.';
    } while (m > 0 || exponent < 1);
    if (isnegative)
        *b++ = '-';
    std::reverse(buffer, b);
    return b;
}

HFFIX_ALWAYS_INLINE void itoa_padded_unchecked(int x, char* b, char* e) noexcept {
    while (e > b) {
        *--e = static_cast<char>('0' + (x % 10));
        x /= 10;
    }
}

inline bool atodate(char const* begin, char const* end, int& year, int& month, int& day) {
    if (end - begin != 8)
        return false;
    std::uint32_t yyyymmdd = details::parse_eight_digits(begin);
    year = static_cast<int>(yyyymmdd / 10000U);
    month = static_cast<int>((yyyymmdd / 100U) % 100U);
    day = static_cast<int>(yyyymmdd % 100U);
    return true;
}

inline bool atotime(
    char const* begin, char const* end, int& hour, int& minute, int& second, int& millisecond) {
    if (end - begin != 8 && end - begin != 12)
        return false;
    hour = static_cast<int>(details::parse_two_digits(begin));
    minute = static_cast<int>(details::parse_two_digits(begin + 3));
    second = static_cast<int>(details::parse_two_digits(begin + 6));
    if (end - begin == 12) {
        millisecond = static_cast<int>(details::parse_two_digits(begin + 9)) * 10 +
                      static_cast<int>(begin[11] - '0');
    } else {
        millisecond = 0;
    }
    return true;
}

inline bool atotime_nano(
    char const* begin, char const* end, int& hour, int& minute, int& second, int& nanosecond) {
    if (end - begin < 8)
        return false;

    hour = static_cast<int>(details::parse_two_digits(begin));
    minute = static_cast<int>(details::parse_two_digits(begin + 3));
    second = static_cast<int>(details::parse_two_digits(begin + 6));

    switch (end - begin) {
        case 12:  // .sss
            nanosecond = static_cast<int>(details::parse_two_digits(begin + 9) * 10U +
                                          std::uint32_t(begin[11] - '0')) *
                         1000000L;
            break;
        case 15:  // .ssssss
            nanosecond = static_cast<int>(details::parse_four_digits(begin + 9) * 100U +
                                          details::parse_two_digits(begin + 13)) *
                         1000L;
            break;
        case 18:  // .sssssssss
            nanosecond = static_cast<int>(details::parse_eight_digits(begin + 9) * 10U +
                                          std::uint32_t(begin[17] - '0'));
            break;
        default:
            return false;
    }
    return true;
}

template <typename T>
struct is_time_point : std::false_type {};

template <typename Clock, typename Duration>
struct is_time_point<std::chrono::time_point<Clock, Duration>> : std::true_type {};

// Past year 2200, days_since_epoch * 86400 * 1e9 overflows int64.
inline constexpr int kMinSupportedYear = 1970;
inline constexpr int kMaxSupportedYear = 2200;

template <typename TimePoint>
inline typename std::enable_if<details::is_time_point<TimePoint>::value, bool>::type atotimepoint(
    char const* begin, char const* end, TimePoint& tp) {
    if (end - begin < 9)
        return false;
    int year, month, day, hour, minute, second, millisecond;
    if (!atotime(begin + 9, end, hour, minute, second, millisecond))
        return false;
    if (!atodate(begin, begin + 8, year, month, day))
        return false;
    if (year < kMinSupportedYear || year > kMaxSupportedYear || month < 1 || month > 12 ||
        day < 1 || day > 31)
        return false;

    // from http://howardhinnant.github.io/date_algorithms.html
    year -= month <= 2;
    unsigned const era = static_cast<unsigned>(year) / 400u;
    unsigned const yoe = static_cast<unsigned>(year) - era * 400u;
    unsigned const doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    std::int64_t const days_since_epoch =
        static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;

    tp = TimePoint(std::chrono::seconds(days_since_epoch * 86400) + std::chrono::hours(hour) +
                   std::chrono::minutes(minute) + std::chrono::seconds(second) +
                   std::chrono::milliseconds(millisecond));

    return true;
}

template <typename TimePoint>
inline typename std::enable_if<details::is_time_point<TimePoint>::value, bool>::type atotimepoint_nano(
    char const* begin, char const* end, TimePoint& tp) {
    if (end - begin < 9)
        return false;
    int year, month, day, hour, minute, second, nanosecond;
    if (!atotime_nano(begin + 9, end, hour, minute, second, nanosecond))
        return false;
    if (!atodate(begin, begin + 8, year, month, day))
        return false;
    if (year < kMinSupportedYear || year > kMaxSupportedYear || month < 1 || month > 12 ||
        day < 1 || day > 31)
        return false;

    // from http://howardhinnant.github.io/date_algorithms.html
    year -= month <= 2;
    unsigned const era = static_cast<unsigned>(year) / 400u;
    unsigned const yoe = static_cast<unsigned>(year) - era * 400u;
    unsigned const doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    std::int64_t const days_since_epoch =
        static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;

    tp = TimePoint(std::chrono::seconds(days_since_epoch * 86400) + std::chrono::hours(hour) +
                   std::chrono::minutes(minute) + std::chrono::seconds(second) +
                   std::chrono::nanoseconds(nanosecond));

    return true;
}

template <typename TimePoint>
inline typename std::enable_if<details::is_time_point<TimePoint>::value, void>::type timepointtoparts(
    TimePoint tp,
    int& year,
    int& month,
    int& day,
    int& hour,
    int& minute,
    int& second,
    int& millisecond) noexcept {
    auto epoch_sec =
        std::chrono::time_point_cast<std::chrono::seconds>(tp).time_since_epoch().count();
    auto day_sec = epoch_sec - (epoch_sec % 86400);
    auto days_since_epoch = day_sec / 86400;

    // see http://howardhinnant.github.io/date_algorithms.html
    days_since_epoch += 719468;
    unsigned const era =
        (days_since_epoch >= 0 ? days_since_epoch : days_since_epoch - 146096) / 146097;
    unsigned const doe = static_cast<unsigned>(days_since_epoch - era * 146097);
    unsigned const yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    year = static_cast<unsigned>(yoe) + era * 400;
    unsigned const doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned const mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp + (mp < 10 ? 3 : -9);
    year += month <= 2;

    auto in_day = tp - std::chrono::seconds(day_sec);
    millisecond =
        std::chrono::time_point_cast<std::chrono::milliseconds>(in_day).time_since_epoch().count();
    hour = millisecond / (60 * 60 * 1000);
    millisecond -= hour * 60 * 60 * 1000;
    minute = millisecond / (60 * 1000);
    millisecond -= minute * 60 * 1000;
    second = millisecond / 1000;
    millisecond -= second * 1000;
}

template <typename TimePoint>
inline typename std::enable_if<details::is_time_point<TimePoint>::value, void>::type
timepointtoparts_nano(TimePoint tp,
                      int& year,
                      int& month,
                      int& day,
                      int& hour,
                      int& minute,
                      int& second,
                      int& nanosecond) noexcept {
    auto epoch_sec =
        std::chrono::time_point_cast<std::chrono::seconds>(tp).time_since_epoch().count();
    auto day_sec = epoch_sec - (epoch_sec % 86400);
    auto days_since_epoch = day_sec / 86400;

    // see http://howardhinnant.github.io/date_algorithms.html
    days_since_epoch += 719468;
    unsigned const era =
        (days_since_epoch >= 0 ? days_since_epoch : days_since_epoch - 146096) / 146097;
    unsigned const doe = static_cast<unsigned>(days_since_epoch - era * 146097);
    unsigned const yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    year = static_cast<unsigned>(yoe) + era * 400;
    unsigned const doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned const mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp + (mp < 10 ? 3 : -9);
    year += month <= 2;

    // the math here must be in higher precision, but at the end it fits in an int
    auto in_day = tp - std::chrono::seconds(day_sec);
    long lnanosecond =
        std::chrono::time_point_cast<std::chrono::nanoseconds>(in_day).time_since_epoch().count();
    hour = lnanosecond / (60 * 60 * 1000000000L);
    lnanosecond -= hour * 60 * 60 * 1000000000L;
    minute = lnanosecond / (60 * 1000000000L);
    lnanosecond -= minute * 60 * 1000000000L;
    second = lnanosecond / 1000000000L;
    lnanosecond -= second * 1000000000L;
    nanosecond = static_cast<int>(lnanosecond);
}

}  // namespace details

/* @endcond*/

/**
 * \brief noexcept FIX writer. Overflow sets error flag; push_back_* no-op
 * afterwards. Caller checks push_back_trailer() return.
 */
class message_writer {
public:
    explicit message_writer(std::span<char> buffer) noexcept
        : buffer_(buffer.data()), buffer_end_(buffer.data() + buffer.size()), next_(buffer.data()) {}

    message_writer(char* buffer, std::size_t size) noexcept
        : message_writer(std::span<char>(buffer, size)) {}

    message_writer(char* begin, char* end) noexcept
        : message_writer(std::span<char>(begin, static_cast<std::size_t>(end - begin))) {}

    template <std::size_t N>
    explicit message_writer(char (&buffer)[N]) noexcept
        : buffer_(buffer), buffer_end_(buffer + N), next_(buffer) {}

    [[nodiscard]] bool ok() const noexcept { return !error_; }

    bool has_error() const noexcept { return error_; }

    char* message_begin() const noexcept { return buffer_; }

    char* message_end() const noexcept { return next_; }

    std::size_t message_size() const noexcept { return static_cast<std::size_t>(next_ - buffer_); }

    std::size_t buffer_size() const noexcept {
        return static_cast<std::size_t>(buffer_end_ - buffer_);
    }

    std::size_t buffer_size_remaining() const noexcept {
        return static_cast<std::size_t>(buffer_end_ - next_);
    }

    template <bool CalculateChecksum = true>
    [[nodiscard]] bool push_back_trailer() noexcept {
        if (error_)
            return false;
        if (!body_length_) {
            error_ = true;
            return false;
        }
        std::size_t const len = static_cast<std::size_t>(next_ - (body_length_ + 7));
        // BodyLength is 6 digits; >999999 would wrap in itoa_padded_unchecked.
        if (len > 999999u) [[unlikely]] {
            error_ = true;
            return false;
        }
        details::itoa_padded_unchecked(static_cast<int>(len), body_length_, body_length_ + 6);
        if (buffer_end_ - next_ < 7) {
            error_ = true;
            return false;
        }
        if constexpr (CalculateChecksum) {
            using std::uint8_t;
            uint8_t const checksum = details::checksum_bytes(buffer_, next_);
            std::memcpy(next_, "10=", 3);
            next_ += 3;
            next_[0] = static_cast<char>('0' + ((checksum / 100) % 10));
            next_[1] = static_cast<char>('0' + ((checksum / 10) % 10));
            next_[2] = static_cast<char>('0' + (checksum % 10));
            next_ += 3;
            *next_++ = '\x01';
        } else {
            std::memcpy(next_, "10=000\x01", 7);
            next_ += 7;
        }
        return true;
    }

    void push_back_header(char const* begin, char const* end) noexcept {
        if (error_)
            return;
        if (body_length_) {
            error_ = true;
            return;
        }
        std::ptrdiff_t const vlen = end - begin;
        if (buffer_end_ - next_ < 2 + vlen + 3 + 7) {
            error_ = true;
            return;
        }
        std::memcpy(next_, "8=", 2);
        next_ += 2;
        std::memcpy(next_, begin, static_cast<std::size_t>(vlen));
        next_ += vlen;
        *next_++ = '\x01';
        std::memcpy(next_, "9=", 2);
        next_ += 2;
        body_length_ = next_;
        next_ += 6;
        *next_++ = '\x01';
    }

    /** String-literal overload; runtime char const* must be wrapped in std::string_view. */
    template <std::size_t N>
    void push_back_header(char const (&literal)[N]) noexcept {
        static_assert(N > 0);
        push_back_header(literal, literal + (N - 1));
    }

    void push_back_header(std::string_view v) noexcept {
        push_back_header(v.data(), v.data() + v.size());
    }

    void push_back_string(int tag, char const* begin, char const* end) noexcept {
        if (error_)
            return;
        std::ptrdiff_t const slen = end - begin;
        std::ptrdiff_t const need = details::max_ascii_chars<int> + 1 + slen + 1;
        if (buffer_end_ - next_ < need) {
            error_ = true;
            return;
        }
        next_ = details::utoa_unchecked(static_cast<unsigned>(tag), next_);
        *next_++ = '=';
        std::memcpy(next_, begin, static_cast<std::size_t>(slen));
        next_ += slen;
        *next_++ = '\x01';
    }

    /** String-literal overload; runtime char const* must be wrapped in std::string_view. */
    template <std::size_t N>
    void push_back_string(int tag, char const (&literal)[N]) noexcept {
        static_assert(N > 0);
        push_back_string(tag, literal, literal + (N - 1));
    }

    void push_back_string(int tag, std::string_view s) noexcept {
        push_back_string(tag, s.data(), s.data() + s.size());
    }

    void push_back_char(int tag, char c) noexcept {
        if (error_)
            return;
        std::ptrdiff_t const need = details::max_ascii_chars<int> + 1 + 1 + 1;
        if (buffer_end_ - next_ < need) {
            error_ = true;
            return;
        }
        next_ = details::utoa_unchecked(static_cast<unsigned>(tag), next_);
        *next_++ = '=';
        *next_++ = c;
        *next_++ = '\x01';
    }

    template <class Int_type>
    void push_back_int(int tag, Int_type n) noexcept {
        if (error_)
            return;
        std::ptrdiff_t const need =
            details::max_ascii_chars<int> + 1 + details::max_ascii_chars<Int_type> + 1;
        if (buffer_end_ - next_ < need) {
            error_ = true;
            return;
        }
        next_ = details::utoa_unchecked(static_cast<unsigned>(tag), next_);
        *next_++ = '=';
        next_ = details::itoa_unchecked(n, next_);
        *next_++ = '\x01';
    }

    template <class Int_type>
    void push_back_decimal(int tag, Int_type mantissa, Int_type exponent) noexcept {
        if (error_)
            return;
        std::ptrdiff_t const need =
            details::max_ascii_chars<int> + 1 + details::max_ascii_chars<Int_type> + 1 + 1;
        if (buffer_end_ - next_ < need) {
            error_ = true;
            return;
        }
        next_ = details::utoa_unchecked(static_cast<unsigned>(tag), next_);
        *next_++ = '=';
        next_ = details::dtoa_unchecked(mantissa, exponent, next_);
        *next_++ = '\x01';
    }

    void push_back_date(int tag, int y, int m, int d) noexcept {
        if (error_)
            return;
        std::ptrdiff_t const need = details::max_ascii_chars<int> + 1 + 8 + 1;
        if (buffer_end_ - next_ < need) {
            error_ = true;
            return;
        }
        next_ = details::utoa_unchecked(static_cast<unsigned>(tag), next_);
        *next_++ = '=';
        details::itoa_padded_unchecked(y, next_, next_ + 4);
        next_ += 4;
        details::itoa_padded_unchecked(m, next_, next_ + 2);
        next_ += 2;
        details::itoa_padded_unchecked(d, next_, next_ + 2);
        next_ += 2;
        *next_++ = '\x01';
    }

    void push_back_monthyear(int tag, int y, int m) noexcept {
        if (error_)
            return;
        std::ptrdiff_t const need = details::max_ascii_chars<int> + 1 + 6 + 1;
        if (buffer_end_ - next_ < need) {
            error_ = true;
            return;
        }
        next_ = details::utoa_unchecked(static_cast<unsigned>(tag), next_);
        *next_++ = '=';
        details::itoa_padded_unchecked(y, next_, next_ + 4);
        next_ += 4;
        details::itoa_padded_unchecked(m, next_, next_ + 2);
        next_ += 2;
        *next_++ = '\x01';
    }

    void push_back_timeonly(int tag, int h, int m, int s) noexcept {
        if (error_)
            return;
        std::ptrdiff_t const need = details::max_ascii_chars<int> + 1 + 8 + 1;
        if (buffer_end_ - next_ < need) {
            error_ = true;
            return;
        }
        next_ = details::utoa_unchecked(static_cast<unsigned>(tag), next_);
        *next_++ = '=';
        details::itoa_padded_unchecked(h, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        details::itoa_padded_unchecked(m, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        details::itoa_padded_unchecked(s, next_, next_ + 2);
        next_ += 2;
        *next_++ = '\x01';
    }

    void push_back_timeonly(int tag, int h, int m, int s, int ms) noexcept {
        if (error_)
            return;
        std::ptrdiff_t const need = details::max_ascii_chars<int> + 1 + 12 + 1;
        if (buffer_end_ - next_ < need) {
            error_ = true;
            return;
        }
        next_ = details::utoa_unchecked(static_cast<unsigned>(tag), next_);
        *next_++ = '=';
        details::itoa_padded_unchecked(h, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        details::itoa_padded_unchecked(m, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        details::itoa_padded_unchecked(s, next_, next_ + 2);
        next_ += 2;
        *next_++ = '.';
        details::itoa_padded_unchecked(ms, next_, next_ + 3);
        next_ += 3;
        *next_++ = '\x01';
    }

    template <class Rep, class Period>
    void push_back_timeonly(int tag, std::chrono::duration<Rep, Period> t) noexcept {
        using namespace std::chrono;
        push_back_timeonly(tag,
                           static_cast<int>(duration_cast<hours>(t).count()),
                           static_cast<int>(duration_cast<minutes>(t % hours(1)).count()),
                           static_cast<int>(duration_cast<seconds>(t % minutes(1)).count()),
                           static_cast<int>(duration_cast<milliseconds>(t % seconds(1)).count()));
    }

    void push_back_timeonly_nano(int tag, int h, int m, int s, int ns) noexcept {
        if (error_)
            return;
        std::ptrdiff_t const need = details::max_ascii_chars<int> + 1 + 18 + 1;
        if (buffer_end_ - next_ < need) {
            error_ = true;
            return;
        }
        next_ = details::utoa_unchecked(static_cast<unsigned>(tag), next_);
        *next_++ = '=';
        details::itoa_padded_unchecked(h, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        details::itoa_padded_unchecked(m, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        details::itoa_padded_unchecked(s, next_, next_ + 2);
        next_ += 2;
        *next_++ = '.';
        details::itoa_padded_unchecked(ns, next_, next_ + 9);
        next_ += 9;
        *next_++ = '\x01';
    }

    template <class Rep, class Period>
    void push_back_timeonly_nano(int tag, std::chrono::duration<Rep, Period> t) noexcept {
        using namespace std::chrono;
        push_back_timeonly_nano(tag,
                                static_cast<int>(duration_cast<hours>(t).count()),
                                static_cast<int>(duration_cast<minutes>(t % hours(1)).count()),
                                static_cast<int>(duration_cast<seconds>(t % minutes(1)).count()),
                                static_cast<int>(duration_cast<nanoseconds>(t % seconds(1)).count()));
    }

    void push_back_timestamp(int tag, int y, int mo, int d, int h, int mi, int s) noexcept {
        if (error_)
            return;
        std::ptrdiff_t const need = details::max_ascii_chars<int> + 1 + 17 + 1;
        if (buffer_end_ - next_ < need) {
            error_ = true;
            return;
        }
        next_ = details::utoa_unchecked(static_cast<unsigned>(tag), next_);
        *next_++ = '=';
        details::itoa_padded_unchecked(y, next_, next_ + 4);
        next_ += 4;
        details::itoa_padded_unchecked(mo, next_, next_ + 2);
        next_ += 2;
        details::itoa_padded_unchecked(d, next_, next_ + 2);
        next_ += 2;
        *next_++ = '-';
        details::itoa_padded_unchecked(h, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        details::itoa_padded_unchecked(mi, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        details::itoa_padded_unchecked(s, next_, next_ + 2);
        next_ += 2;
        *next_++ = '\x01';
    }

    void push_back_timestamp(int tag, int y, int mo, int d, int h, int mi, int s, int ms) noexcept {
        if (error_)
            return;
        std::ptrdiff_t const need = details::max_ascii_chars<int> + 1 + 21 + 1;
        if (buffer_end_ - next_ < need) {
            error_ = true;
            return;
        }
        next_ = details::utoa_unchecked(static_cast<unsigned>(tag), next_);
        *next_++ = '=';
        details::itoa_padded_unchecked(y, next_, next_ + 4);
        next_ += 4;
        details::itoa_padded_unchecked(mo, next_, next_ + 2);
        next_ += 2;
        details::itoa_padded_unchecked(d, next_, next_ + 2);
        next_ += 2;
        *next_++ = '-';
        details::itoa_padded_unchecked(h, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        details::itoa_padded_unchecked(mi, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        details::itoa_padded_unchecked(s, next_, next_ + 2);
        next_ += 2;
        *next_++ = '.';
        details::itoa_padded_unchecked(ms, next_, next_ + 3);
        next_ += 3;
        *next_++ = '\x01';
    }

    template <class Clock, class Duration>
    void push_back_timestamp(int tag, std::chrono::time_point<Clock, Duration> tp) noexcept {
        int year, month, day, hour, minute, second, millisecond;
        details::timepointtoparts(tp, year, month, day, hour, minute, second, millisecond);
        push_back_timestamp(tag, year, month, day, hour, minute, second, millisecond);
    }

    void push_back_timestamp_nano(int tag, int y, int mo, int d, int h, int mi, int s, int ns) noexcept {
        if (error_)
            return;
        std::ptrdiff_t const need = details::max_ascii_chars<int> + 1 + 27 + 1;
        if (buffer_end_ - next_ < need) {
            error_ = true;
            return;
        }
        next_ = details::utoa_unchecked(static_cast<unsigned>(tag), next_);
        *next_++ = '=';
        details::itoa_padded_unchecked(y, next_, next_ + 4);
        next_ += 4;
        details::itoa_padded_unchecked(mo, next_, next_ + 2);
        next_ += 2;
        details::itoa_padded_unchecked(d, next_, next_ + 2);
        next_ += 2;
        *next_++ = '-';
        details::itoa_padded_unchecked(h, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        details::itoa_padded_unchecked(mi, next_, next_ + 2);
        next_ += 2;
        *next_++ = ':';
        details::itoa_padded_unchecked(s, next_, next_ + 2);
        next_ += 2;
        *next_++ = '.';
        details::itoa_padded_unchecked(ns, next_, next_ + 9);
        next_ += 9;
        *next_++ = '\x01';
    }

    template <class Clock, class Duration>
    void push_back_timestamp_nano(int tag, std::chrono::time_point<Clock, Duration> tp) noexcept {
        int year, month, day, hour, minute, second, nanosecond;
        details::timepointtoparts_nano(tp, year, month, day, hour, minute, second, nanosecond);
        push_back_timestamp_nano(tag, year, month, day, hour, minute, second, nanosecond);
    }

    /** \brief UTCTimestamp from raw signed epoch milliseconds. */
    void push_back_timestamp_epoch_millis(int tag, std::int64_t epoch_millis) noexcept {
        push_back_timestamp(tag,
                            std::chrono::sys_time<std::chrono::milliseconds>{
                                std::chrono::milliseconds{epoch_millis}});
    }

    /** \brief UTCTimestamp from raw signed epoch nanoseconds. */
    void push_back_timestamp_epoch_nanos(int tag, std::int64_t epoch_nanos) noexcept {
        push_back_timestamp_nano(
            tag,
            std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{epoch_nanos}});
    }

    void push_back_data(int tag_data_length, int tag_data, char const* begin, char const* end) noexcept {
        if (error_)
            return;
        if (end < begin) {
            error_ = true;
            return;
        }
        std::ptrdiff_t const dlen = end - begin;
        std::ptrdiff_t const need = details::max_ascii_chars<int> + 1 +
                                    details::max_ascii_chars<int> + 1 +
                                    details::max_ascii_chars<int> + 1 + dlen + 1;
        if (buffer_end_ - next_ < need) {
            error_ = true;
            return;
        }
        next_ = details::utoa_unchecked(static_cast<unsigned>(tag_data_length), next_);
        *next_++ = '=';
        next_ = details::itoa_unchecked(static_cast<int>(dlen), next_);
        *next_++ = '\x01';
        next_ = details::utoa_unchecked(static_cast<unsigned>(tag_data), next_);
        *next_++ = '=';
        std::memcpy(next_, begin, static_cast<std::size_t>(dlen));
        next_ += dlen;
        *next_++ = '\x01';
    }

private:
    char* buffer_;
    char* buffer_end_;
    char* next_;
    char* body_length_ = nullptr;
    bool error_ = false;
};

/**
 * \brief Build one FIX message via `body(writer)` and push_back_trailer. Returns
 * `false` on overflow; `end_out` set to one past the last byte on success.
 */
template <class F>
[[nodiscard]] inline bool try_write_message(std::span<char> buffer, char*& end_out, F body) noexcept {
    message_writer w(buffer);
    body(w);
    if (!w.push_back_trailer())
        return false;
    end_out = w.message_end();
    return true;
}

template <class F>
[[nodiscard]] inline bool try_write_message(char* begin, char* end, char*& end_out, F body) noexcept {
    return try_write_message(
        std::span<char>(begin, static_cast<std::size_t>(end - begin)), end_out, std::move(body));
}

class basic_message_reader;
class basic_message_reader_const_iterator;
class basic_indexed_message;
class basic_group_entry;
class basic_group_iterator;
class basic_group_view;

namespace groups {
/**
 * \brief Compile-time mapping from a NoXxx count tag to its delimiter tag.
 *
 * Specialize via `HFFIX_REGISTER_GROUP(CountTag, FirstTag)`. Unknown counts
 * yield `first_tag == 0` and trigger `static_assert` on `reader.group<CountTag>()`.
 */
template <int CountTag>
struct group_def {
    static constexpr int first_tag = 0;
};
}  // namespace groups

#define HFFIX_REGISTER_GROUP(count_tag_name, first_tag_name)           \
    namespace hffix {                                                  \
    namespace groups {                                                 \
    template <>                                                        \
    struct group_def<::hffix::tag::count_tag_name> {                   \
        static constexpr int first_tag = ::hffix::tag::first_tag_name; \
    };                                                                 \
    }                                                                  \
    }                                                                  \
    static_assert(true, "require trailing semicolon")

/**
 * \brief FIX field value for hffix::basic_message_reader.
 *
 * <h3>Usage</h3>
 *
 * This class is a range `begin(),end()` of pointers into
 * a `basic_message_reader` buffer which delimit the value for one field.
 *
 * FIX field values are an array of chars, and are usually ASCII.
 * Type conversion deserialization is provided by the `as_` family
 * of methods.
 *
 * <h3>Extension</h3>
 *
 * Keep in mind that if you don't like the way any of the the `as_` methods
 * perform deserialization for a type, then you can deserialize the field value
 * yourself, by reading the string delimited by `begin(),end()`.
 *
*/
class field_value {
public:
    char const* begin() const { return begin_; }

    char const* end() const { return end_; }

    /** \brief Size of the field value, in bytes. */
    size_t size() const { return end_ - begin_; }

    inline friend bool operator==(field_value const& that, char const* cstring) {
        return !strncmp(that.begin(), cstring, that.size()) && !cstring[that.size()];
    }

    inline friend bool operator==(char const* cstring, field_value const& that) {
        return that == cstring;
    }

    inline friend bool operator!=(field_value const& that, char const* cstring) {
        return !(that == cstring);
    }

    inline friend bool operator!=(char const* cstring, field_value const& that) {
        return !(that == cstring);
    }

    inline friend bool operator==(field_value const& that, std::string_view s) {
        return std::equal(that.begin(), that.end(), s.begin(), s.end());
    }

    inline friend bool operator==(std::string_view s, field_value const& that) { return that == s; }

    inline friend bool operator!=(field_value const& that, std::string_view s) {
        return !(that == s);
    }

    inline friend bool operator!=(std::string_view s, field_value const& that) {
        return !(that == s);
    }

    friend std::ostream& operator<<(std::ostream& os, field_value const& that) {
        return os.write(that.begin(), that.size());
    }

    /** \name String Conversion Methods */
    //@{

    [[nodiscard]] std::string_view as_string_view() const {
        return std::string_view(begin(), size());
    }

    /**
     * \brief First byte of the field value. Reads `*begin()` unconditionally.
     *
     * \warning Undefined behavior when `size() == 0`. FIX char-type
     * fields are always one byte by spec, so on conforming input from
     * a validated reader this cannot fire; untrusted callers must
     * check `size() > 0` first.
     */
    [[nodiscard]] char as_char() const { return *begin(); }

    //@}

    /** \name Decimal Float Conversion Methods */
    //@{

    /**
    \brief Non-validating ascii-to-decimal. Decimal float as
    `mantissa * 10^exponent`, non-normalized, `exponent <= 0`.

    \warning **Trusted-input only.** Behavior is undefined for any of
    the following on the input range `[begin(), end())`:

    - any byte outside `'0'..'9'` other than a single leading `-` or a
      single `.`,
    - more than one `.`,
    - an empty value (zero bytes),
    - a digit count large enough that `Int_type` overflows; no
      overflow detection is performed and the result silently wraps.

    Wire-untrusted values must use `try_as_decimal` instead. This
    function exists for the hot path after the caller has externally
    validated the byte range (e.g. from a fixed-format venue feed with
    schema-checked fields).

    \tparam Int_type Signed integer type for mantissa and exponent.
    \param[out] mantissa Integer mantissa.
    \param[out] exponent Decimal exponent, `<= 0`.
    */
    template <typename Int_type>
    void as_decimal_unchecked(Int_type& mantissa, Int_type& exponent) const {
        details::atod<Int_type>(begin(), end(), mantissa, exponent);
    }

    /**
    \brief Validating ascii-to-decimal conversion. Rejects empty value,
    multiple dots, stray '+', non-digit chars. No overflow detection.
    Sets out-params only on success.
    \return True on success, false otherwise.
    */
    template <typename Int_type>
    [[nodiscard]] bool try_as_decimal(Int_type& mantissa, Int_type& exponent) const noexcept {
        return details::try_atod<Int_type>(begin(), end(), mantissa, exponent);
    }

    //@}

private:
    template <typename Int_type, bool Is_signed_integer>
    struct as_int_selector {};

    template <typename Int_type>
    struct as_int_selector<Int_type, true> {
        static Int_type call_as_int(char const* begin, char const* end) {
            return details::atoi<Int_type>(begin, end);
        }
    };

    template <typename Int_type>
    struct as_int_selector<Int_type, false> {
        static Int_type call_as_int(char const* begin, char const* end) {
            return details::atou<Int_type>(begin, end);
        }
    };

public:
    /** \name Integer Conversion Methods */
    //@{

    /**
    \brief Non-validating ascii-to-integer.

    \warning **Trusted-input only.** Behavior is undefined for any of
    the following on the input range `[begin(), end())`:

    - any byte outside `'0'..'9'` other than a single leading `-` on a
      signed `Int_type`,
    - a stray `+` (no positive sign permitted),
    - an empty value (zero bytes),
    - a digit count large enough to overflow `Int_type`; no overflow
      detection is performed and the result silently wraps.

    Wire-untrusted values must use `try_as_int` instead. This function
    exists for the hot path after the caller has externally validated
    the byte range. Marked `HFFIX_HOT` because it appears in
    parser-inner-loop call sites (`CheckSum`, `BodyLength`,
    `MsgSeqNum`, post-validated tag accumulators).

    \tparam Int_type Signed or unsigned integer type.
    \return The parsed value. Undefined for inputs that violate the
    preconditions above.
    */
    template <typename Int_type>
    [[nodiscard]] HFFIX_HOT Int_type as_int_unchecked() const {
        return as_int_selector<Int_type, std::numeric_limits<Int_type>::is_signed>::call_as_int(
            begin(), end());
    }

    /**
    \brief Validating ascii-to-integer conversion.

    Rejects an empty value, a stray '+', and any non-digit other than a
    leading '-' on a signed Int_type. No overflow detection.

    \tparam Int_type Signed or unsigned integer type.
    \param[out] out Set only if the parse succeeded.
    \return True on success, false otherwise.
    */
    template <typename Int_type>
    [[nodiscard]] bool try_as_int(Int_type& out) const noexcept {
        if constexpr (std::numeric_limits<Int_type>::is_signed)
            return details::try_atoi<Int_type>(begin(), end(), out);
        else
            return details::try_atou<Int_type>(begin(), end(), out);
    }

    //@}

    /** \name Date and Time Conversion Methods */
    //@{

    /**
    \brief Parse a LocalMktDate or UTCDate `YYYYMMDD` field.
    Out-params set only on success.
    */
    [[nodiscard]] bool as_date(int& year, int& month, int& day) const {
        return details::atodate(begin(), end(), year, month, day);
    }

    /**
    \brief Parse a MonthYear `YYYYMM` field.

    Validates length only (6 bytes). The 6 bytes are then parsed with
    the unchecked digit parser, so non-digit content yields undefined
    output but cannot read past the field. Callers receiving values
    from untrusted sources should additionally range-check
    `month ∈ [1, 12]` and `year` against a venue-plausible band after
    this returns `true`.

    \return `true` only if `size() == 6`; out-params unmodified on `false`.
    */
    [[nodiscard]] bool as_monthyear(int& year, int& month) const {
        if (end() - begin() != 6)
            return false;

        year = details::atoi<int>(begin(), begin() + 4);
        month = details::atoi<int>(begin() + 4, begin() + 6);

        return true;
    }

    /**
    \brief Parse a UTCTimeOnly `HH:MM:SS[.sss]` field. Out-params set
    only on success.
    */
    [[nodiscard]] bool as_timeonly(int& hour, int& minute, int& second, int& millisecond) const {
        return details::atotime(begin(), end(), hour, minute, second, millisecond);
    }

    /**
    \brief Parse a UTCTimeOnly field with nanosecond precision
    (`HH:MM:SS[.sss|.ssssss|.sssssssss]`). Out-params set only on
    success.
    */
    [[nodiscard]] bool as_timeonly_nano(int& hour, int& minute, int& second, int& nanosecond) const {
        return details::atotime_nano(begin(), end(), hour, minute, second, nanosecond);
    }

    /**
    \brief Parse a UTCTimestamp `YYYYMMDD-HH:MM:SS[.sss]` field.
    Date-part length is checked via short-circuit `&&` after the time
    part parses. Out-params set only on success.
    */
    [[nodiscard]] bool as_timestamp(
        int& year, int& month, int& day, int& hour, int& minute, int& second, int& millisecond) const {
        return details::atotime(begin() + 9, end(), hour, minute, second, millisecond) &&
               details::atodate(begin(), begin() + 8, year, month, day);
    }

    /**
    \brief Parse a UTCTimestamp field with nanosecond precision.
    Date-part length is checked via short-circuit `&&` after the time
    part parses. Out-params set only on success.
    */
    [[nodiscard]] bool as_timestamp_nano(
        int& year, int& month, int& day, int& hour, int& minute, int& second, int& nanosecond) const {
        return details::atotime_nano(begin() + 9, end(), hour, minute, second, nanosecond) &&
               details::atodate(begin(), begin() + 8, year, month, day);
    }

    //@}

    /** \brief UTCTimestamp as signed epoch nanoseconds, or nullopt. */
    [[nodiscard]] std::optional<std::int64_t> as_epoch_nanos() const noexcept {
        std::chrono::sys_time<std::chrono::nanoseconds> tp;
        if (!details::atotimepoint_nano(begin(), end(), tp))
            return std::nullopt;
        return tp.time_since_epoch().count();
    }

    /** \brief UTCTimestamp as signed epoch milliseconds, or nullopt. */
    [[nodiscard]] std::optional<std::int64_t> as_epoch_millis() const noexcept {
        std::chrono::sys_time<std::chrono::milliseconds> tp;
        if (!details::atotimepoint(begin(), end(), tp))
            return std::nullopt;
        return tp.time_since_epoch().count();
    }

    /** \name std::chrono Date and Time Conversion Methods */
    //@{

    /**
    \brief Parse a UTCTimestamp field into a `std::chrono::time_point`.
    Uses Howard Hinnant's proleptic Gregorian algorithms (see
    `http://howardhinnant.github.io/date_algorithms.html`).
    */
    template <typename Clock, typename Duration>
    [[nodiscard]] bool as_timestamp(std::chrono::time_point<Clock, Duration>& tp) const {
        return details::atotimepoint(begin(), end(), tp);
    }

    /**
    \brief Parse a UTCTimestamp field with nanosecond precision into a
    `std::chrono::time_point`.
    */
    template <typename Clock, typename Duration>
    [[nodiscard]] bool as_timestamp_nano(std::chrono::time_point<Clock, Duration>& tp) const {
        return details::atotimepoint_nano(begin(), end(), tp);
    }

    /**
    \brief Parse a UTCTimeOnly field as a `std::chrono::duration`.
    */
    template <typename Rep, typename Period>
    [[nodiscard]] bool as_timeonly(std::chrono::duration<Rep, Period>& dur) const {
        int hour, minute, second, millisecond;
        if (!as_timeonly(hour, minute, second, millisecond))
            return false;
        dur = std::chrono::hours(hour) + std::chrono::minutes(minute) +
              std::chrono::seconds(second) + std::chrono::milliseconds(millisecond);
        return true;
    }

    /**
    \brief Parse a UTCTimeOnly field with nanosecond precision as a
    `std::chrono::duration`.
    */
    template <typename Rep, typename Period>
    [[nodiscard]] bool as_timeonly_nano(std::chrono::duration<Rep, Period>& dur) const {
        int hour, minute, second, nanosecond;
        if (!as_timeonly_nano(hour, minute, second, nanosecond))
            return false;
        dur = std::chrono::hours(hour) + std::chrono::minutes(minute) +
              std::chrono::seconds(second) + std::chrono::nanoseconds(nanosecond);
        return true;
    }

    //@}

private:
    friend class field;
    friend class basic_message_reader_const_iterator;
    friend class basic_message_reader;
    friend class basic_indexed_message;
    char const* begin_ = nullptr;
    char const* end_ = nullptr;
};

/**
 * \brief A FIX field for hffix::basic_message_reader, with tag and hffix::field_value.
 *
 * This class is the hffix::basic_message_reader::value_type for the hffix::basic_message_reader Container.
 */
class field {
public:
    int tag() const { return tag_; }

    field_value const& value() const { return value_; }

    /** \brief Stream as `tag=value`. */
    friend std::ostream& operator<<(std::ostream& os, field const& that) {
        os << that.tag_ << "=";
        return os.write(that.value_.begin(), that.value_.size());
    }

private:
    friend class basic_message_reader_const_iterator;
    friend class basic_message_reader;
    int tag_ = 0;
    field_value value_;
};

/**
\brief The iterator type for hffix::basic_message_reader.

Satisfies the const Input Iterator Concept for an immutable hffix::basic_message_reader
container of fields.
*/
class basic_message_reader_const_iterator {
public:
    /**
     * \brief Default-constructs an invalid iterator. Not dereferenceable.
     */
    basic_message_reader_const_iterator() {}

private:
    basic_message_reader_const_iterator(basic_message_reader const&, char const* buffer)
        : buffer_(buffer), message_end_(nullptr), current_() {}

public:
    typedef ::std::input_iterator_tag iterator_category;
    typedef field value_type;
    typedef std::ptrdiff_t difference_type;
    typedef field* pointer;
    typedef field& reference;

    field const& operator*() const { return current_; }

    field const* operator->() const { return &current_; }

    /**
    \brief Pointer to the first byte of the current field on the wire.
    */
    char const* buffer_begin() const { return buffer_; }

    friend bool operator==(basic_message_reader_const_iterator const& a,
                           basic_message_reader_const_iterator const& b) {
        return a.buffer_ == b.buffer_;
    }

    friend bool operator!=(basic_message_reader_const_iterator const& a,
                           basic_message_reader_const_iterator const& b) {
        return a.buffer_ != b.buffer_;
    }

    friend bool operator<(basic_message_reader_const_iterator const& a,
                          basic_message_reader_const_iterator const& b) {
        return a.buffer_ < b.buffer_;
    }

    friend bool operator>(basic_message_reader_const_iterator const& a,
                          basic_message_reader_const_iterator const& b) {
        return a.buffer_ > b.buffer_;
    }

    friend bool operator<=(basic_message_reader_const_iterator const& a,
                           basic_message_reader_const_iterator const& b) {
        return a.buffer_ <= b.buffer_;
    }

    friend bool operator>=(basic_message_reader_const_iterator const& a,
                           basic_message_reader_const_iterator const& b) {
        return a.buffer_ >= b.buffer_;
    }

    basic_message_reader_const_iterator operator++(int) {
        basic_message_reader_const_iterator i(*this);
        ++(*this);
        return i;
    }

    basic_message_reader_const_iterator& operator++() {
        increment();
        return *this;
    }

    /**
     * \brief Advance by `addend` fields.
     * \pre `addend >= 0`. Fires `HFFIX_ASSERT` otherwise.
     */
    friend basic_message_reader_const_iterator operator+(basic_message_reader_const_iterator a,
                                                         int addend) {
        HFFIX_ASSERT(addend >= 0,
                     "basic_message_reader::const_iterator is a Forward Iterator, so only "
                     "positive addends are allowed.");
        for (int i = 0; i < addend; ++i)
            ++a;

        return a;
    }

    friend basic_message_reader_const_iterator operator+(int addend,
                                                         basic_message_reader_const_iterator a) {
        return a + addend;
    }

private:
    friend class basic_message_reader;
    char const* buffer_ = nullptr;
    char const* message_end_ = nullptr;
    field current_;

    void increment();
};

/**
 * \brief A predicate constructed with a FIX tag which returns true if the tag of the hffix::field passed to the predicate is equal.
 */
struct tag_equal {
    tag_equal(int tag) : tag(tag) {}

    int tag;

    bool operator()(field const& v) const { return v.tag() == tag; }
};

/**
 * \brief An algorithm similar to `std::find_if` for forward-searching over a range and finding items which match a predicate.
 *
 * Instead of searching from `begin` to `end`, searches from `i` to `end`, then searches from `begin` to `i`.
 * Efficient for finding multiple items when the expected ordering of the items is known.
 *
 * This expression:
 * \code
 * find_with_hint(begin, end, predicate, i)
 * \endcode
 * will behave exactly the same as this expression:
 * \code
 * end != (i = std::find_if(begin, end, predicate))
 * \endcode
 * except for these two differences:
 * * In the first expression, `i` is not modifed if no item is found.
 * * The first expression is faster if the found item is a near successor of `i`.
 *
 * Example usage:
 * \code
 * hffix::basic_message_reader::const_iterator i = reader.begin();
 *
 * if (hffix::find_with_hint(reader.begin(), reader.end(), hffix::tag_equal(hffix::tag::MsgSeqNum), i)
 *   int seqnum = i++->as_int_unchecked<int>();
 *
 * if (hffix::find_with_hint(reader.begin(), reader.end(), hffix::tag_equal(hffix::tag::TargetCompID), i)
 *   std::string_view targetcompid = i++->as_string_view();
 * \endcode
 *
 * See also the convenience method hffix::basic_message_reader::find_with_hint.
 *
 * \param begin The beginning of the range to search.
 * \param end The end of the range to search.
 * \param predicate A predicate which provides function `bool operator() (ForwardIterator::value_type const &v) const`.
 * \param i If an item is found which satisfies `predicate`, then `i` is modified to point to the found item. Else `i` is unmodified.
 * \return True if an item was found which matched `predicate`, and `i` was modified to point to the found item.
 *
 * \note A past-the-end hint costs a full scan, not an early `false`.
 */
template <typename ForwardIterator, typename UnaryPredicate>
[[nodiscard]] inline bool find_with_hint(ForwardIterator begin,
                                         ForwardIterator end,
                                         UnaryPredicate predicate,
                                         ForwardIterator& i) {
    ForwardIterator j = std::find_if(i, end, predicate);
    if (j != end) {
        i = j;
        return true;
    }
    j = std::find_if(begin, i, predicate);
    if (j != i) {
        i = j;
        return true;
    }
    return false;
}

/**
 * \brief One FIX message for reading.
 *
 * An immutable Forward Container of FIX fields. Given a buffer containing a FIX message, the hffix::basic_message_reader
 * will provide an Iterator for iterating over the fields in the message without modifying the buffer. The buffer
 * used to construct the hffix::basic_message_reader must outlive the hffix::basic_message_reader.
 *
 * During construction, hffix::basic_message_reader checks to make sure there is a complete,
 * valid FIX message in the buffer. It looks only at the header and trailer transport fields in the message,
 * not at the content fields, so construction is O(1).
 *
 * If hffix::basic_message_reader is complete and valid after construction,
 * hffix::basic_message_reader::begin() returns an iterator that points to the MsgType field
 * in the FIX Standard Message Header, and
 * hffix::basic_message_reader::end() returns an iterator that points to the CheckSum field in the
 * FIX Standard Message Trailer.
 *
 * The hffix::basic_message_reader will only iterate over content fields of the message, and will skip over all of the framing transport fields
 *  that are mixed in with the content fields in FIX. Here is the list of skipped fields which will not appear when iterating over the fields of the message:
 *
 * - BeginString
 * - BodyLength
 * - CheckSum
 * - And all of the binary data length framing fields listed in hffix::anonymous_namespace{hffix_fields.hpp}::length_fields.
 *
 * Fields of binary data type are content fields, and will be iterated over like any other field.
 * The special FIX binary data length framing fields will be skipped, but the length of the binary data
 * is accessible from the hffix::basic_message_reader::value_type::value().size() of the content field.
*/
class basic_message_reader {
public:
    typedef field value_type;
    typedef field const& const_reference;
    typedef basic_message_reader_const_iterator const_iterator;
    typedef field const* const_pointer;
    typedef size_t size_type;

    /**
    \brief Construct from a contiguous byte span.
    \param buffer Span over the buffer to be read.
    */
    explicit basic_message_reader(std::span<char const> buffer)
        : end_(*this, 0),
          buffer_(buffer.data()),
          buffer_end_(buffer.data() + buffer.size()),
          is_complete_(false),
          is_valid_(true),
          begin_(*this, 0) {
        init();
    }

    /**
    \brief Construct by buffer pointer and size.
    */
    basic_message_reader(char const* buffer, std::size_t size)
        : basic_message_reader(std::span<char const>(buffer, size)) {}

    /**
    \brief Construct by buffer begin and end pointers.
    */
    basic_message_reader(char const* begin, char const* end)
        : basic_message_reader(std::span<char const>(begin, static_cast<std::size_t>(end - begin))) {
    }

    basic_message_reader(basic_message_reader const&) noexcept = default;
    basic_message_reader& operator=(basic_message_reader const&) noexcept = default;
    basic_message_reader(basic_message_reader&&) noexcept = default;
    basic_message_reader& operator=(basic_message_reader&&) noexcept = default;

    /**
     * \brief Construct a basic_message_reader from a message_writer. Equivalent to
     * \code
     * hffix::message_writer w;
     * hffix::basic_message_reader r(w.message_begin(), w.message_end());
     * \endcode
     */
    basic_message_reader(message_writer const& that)
        : end_(*this, 0),
          buffer_(that.message_begin()),
          buffer_end_(that.message_end()),
          is_complete_(false),
          is_valid_(true),
          begin_(*this, 0) {
        init();
    }

    /**
    \brief Construct on an array reference to a buffer.
    \tparam N The size of the array.
    \param buffer An array reference. The reader will read from the entire array of length _N_.
    */
    template <size_t N>
    basic_message_reader(char const (&buffer)[N])
        : end_(*this, 0),
          buffer_(buffer),
          buffer_end_(&(buffer[N])),
          is_complete_(false),
          is_valid_(true),
          begin_(*this, 0) {
        init();
    }

    ~basic_message_reader() {}

    /**
     * \brief True if the buffer contains a complete FIX message.
     */
    [[nodiscard]] bool is_complete() const { return is_complete_; }

    /**
     * \brief True if the message is valid.
     *
     * A valid message must meet these criteria.
     * * The first field is *BeginString*.
     * * The next field is *BodyLength*, and there is a *CheckSum* field at the end of the message at the location dictated by *BodyLength*.
     * * After *BodyLength* there is a *MsgType* field.
     *
     * If false, the message is unintelligable, and the length of the message is unknown.
     *
     * _fix-42_with_errata_20010501.pdf_ p.17:
     * "Valid FIX Message is a message that is properly formed according to this specification and contains a
     * valid body length and checksum field"
     *
    */
    [[nodiscard]] bool is_valid() const { return is_valid_; }

    /**
     * \brief Returns a new basic_message_reader for the next FIX message in the buffer.
     *
     * If this message is_valid() and is_complete(), assume that the next message comes immediately
     * after this one and return a new basic_message_reader constructed at this->message_end().
     *
     * If this message `!`is_valid(), will search the remainder of the buffer
     * for the text "8=FIX", to see if there might be a complete or partial valid message
     * anywhere else in the remainder of the buffer, will return a new basic_message_reader constructed at that location.
     *
     * \pre `is_complete()`. Fires `HFFIX_ASSERT` otherwise.
     */
    basic_message_reader next_message_reader() const {
        HFFIX_ASSERT(is_complete_, "Can't call next_message_reader on an incomplete message.");

        if (!is_valid_) {  // resync by scanning for the next "8=FIX"
            // Guard against `buffer_end_ - 10` forming a pointer below `buffer_`
            // (pointer arithmetic UB) when the buffer is shorter than 10 bytes.
            if (buffer_end_ - buffer_ < 10) {
                return basic_message_reader(buffer_end_, buffer_end_);
            }
            char const* b = buffer_ + 1;
            while (b < buffer_end_ - 10) {
                if (!std::memcmp(b, "8=FIX", 5))
                    break;
                ++b;
            }
            return basic_message_reader(b, buffer_end_);
        }

        char const* next = end_.current_.value_.end_ + 1;
        HFFIX_PREFETCH(next + 64);
        return basic_message_reader(next, buffer_end_);
    }

    /**
    \brief Calculate the checksum for this message.

    Reader never computes the checksum implicitly. Compare against the
    CheckSum field reported by the message:

    \code
    if (r.calculate_check_sum() == r.check_sum()->value().as_int_unchecked<unsigned char>()) {}
    \endcode

    \pre `is_valid()`. Fires `HFFIX_ASSERT` otherwise.
    */
    [[nodiscard]] unsigned char calculate_check_sum() const noexcept {
        HFFIX_ASSERT(is_valid_, "hffix Cannot calculate checksum for an invalid message.");
        return details::checksum_bytes(buffer_, end_.buffer_);
    }

    /** \name Field Access */
    //@{
    /**
    \brief An iterator to the MsgType field in the FIX message. Same as hffix::basic_message_reader::message_type().
    \pre `is_valid()`. Fires `HFFIX_ASSERT` otherwise.
    */
    const_iterator begin() const {
        HFFIX_ASSERT(is_valid_, "hffix Cannot return iterator for an invalid message.");
        return begin_;
    }

    /**
    \brief An iterator to the CheckSum field in the FIX message. Same as hffix::basic_message_reader::check_sum().
    \pre `is_valid()`. Fires `HFFIX_ASSERT` otherwise.
    */
    const_iterator end() const {
        HFFIX_ASSERT(is_valid_, "hffix Cannot return iterator for an invalid message.");
        return end_;
    }

    /**
    \brief An iterator to the MsgType field in the FIX message. Same as hffix::basic_message_reader::begin().
    \pre `is_valid()`. Fires `HFFIX_ASSERT` otherwise.
    */
    const_iterator message_type() const {
        HFFIX_ASSERT(is_valid_, "hffix Cannot return iterator for an invalid message.");
        return begin_;
    }

    /**
    \brief An iterator to the CheckSum field in the FIX message. Same as hffix::basic_message_reader::end().
    \pre `is_valid()`. Fires `HFFIX_ASSERT` otherwise.
    */
    const_iterator check_sum() const {
        HFFIX_ASSERT(is_valid_, "hffix Cannot return iterator for an invalid message.");
        return end_;
    }

    /**
     * \brief Returns the FIX version prefix BeginString field value begin pointer. (Example: "FIXT.1.1")
     */
    char const* prefix_begin() const { return buffer_ + 2; }

    /**
     * \brief Returns the FIX version prefix BeginString field value end pointer.
     */
    char const* prefix_end() const { return prefix_end_; }

    /**
     * \brief Returns the FIX version prefix BeginString field value size. (Example: returns 8 for "FIXT.1.1")
     */
    size_t prefix_size() const { return prefix_end_ - buffer_ - 2; }

    /**
     * \brief Convenient synonym for `hffix::find_with_hint(reader.begin(), reader.end(), hffix::tag_equal(tag), i)`.
     *
     * Similar to `std::find_if`. See `hffix::find_with_hint` for details.
     *
     * \param tag The field tag number to find.
     * \param i If a field is found which has the tag number `tag`, then `i` is modified to point to the found item. Else `i` is unmodified.
     * \return True if a field was found, and `i` was modified to point to the found field.
     *
     * Example usage:
     * \code
     * hffix::basic_message_reader::const_iterator i = reader.begin();
     *
     * if (reader.find_with_hint(MsgSeqNum, i))
     *   int seqnum = i++->as_int_unchecked<int>();
     *
     * if (reader.find_with_hint(TargetCompID, i))
     *   std::string_view targetcompid = i++->as_string_view();
     * \endcode
     */
    [[nodiscard]] inline bool find_with_hint(int tag, const_iterator& i) const {
        return hffix::find_with_hint(begin(), end(), tag_equal(tag), i);
    }

    /**
     * \brief Return a `basic_group_view` over a FIX repeating group.
     *
     * \param count_tag The NoXxx tag that introduces the group.
     * \param first_tag_in_group Delimiter tag (first field of each entry),
     * supplied by the caller from the FIX dictionary.
     *
     * Empty view if `count_tag` is absent or the group is empty. Call
     * `group()` on a `basic_group_entry` for nested groups.
     */
    basic_group_view group(int count_tag, int first_tag_in_group) const;

    /**
     * \brief Compile-time dispatch over a registered FIX group.
     *
     * Delimiter looked up from `hffix::groups::group_def<CountTag>`. All
     * FIX 5.0 SP2 + FIXT 1.1 groups are pre-registered in the generated
     * `hffix_groups.hpp`; extend with `HFFIX_REGISTER_GROUP(...)`.
     */
    template <int CountTag>
    basic_group_view group() const;

    //@}

    /** \name Buffer Access */
    //@{
    /**
    \brief A pointer to the begining of the buffer.

    buffer_begin() == message_begin()
    */
    char const* buffer_begin() const { return buffer_; }

    /**
    \brief A pointer to past-the-end of the buffer.
    */
    char const* buffer_end() const { return buffer_end_; }

    /**
    \brief The size of the buffer in bytes.
    */
    size_t buffer_size() const { return buffer_end_ - buffer_; }

    /**
    \brief A pointer to the beginning of the FIX message in the buffer.

     buffer_begin() == message_begin()
    */
    char const* message_begin() const { return buffer_; }

    /**
    \brief A pointer to past-the-end of the FIX message in the buffer.
    \pre `is_valid()`. Fires `HFFIX_ASSERT` otherwise.
    */
    char const* message_end() const {
        HFFIX_ASSERT(is_valid_, "hffix Cannot determine size of an invalid message.");
        return end_.current_.value_.end_ + 1;
    }

    /**
    \brief The entire size of the FIX message in bytes.
    \pre `is_valid()`. Fires `HFFIX_ASSERT` otherwise.
    */
    size_t message_size() const {
        HFFIX_ASSERT(is_valid_, "hffix Cannot determine size of an invalid message.");
        return end_.current_.value_.end_ - buffer_ + 1;
    }

    //@}

private:
    friend class basic_message_reader_const_iterator;

    void init() {
        // Need at least 9 bytes before forming `buffer_ + 9`; otherwise the pointer
        // arithmetic itself is UB per [expr.add]/4.
        if (buffer_end_ - buffer_ < 9) {
            is_complete_ = false;
            return;
        }
        // BeginString length varies ("8=FIX.4.4", "8=FIXT.1.1", ...). Skip
        // the shortest possible prefix and then scan to the first SOH.
        char const* b = buffer_ + 9;

        while (true) {
            if (b >= buffer_end_) {
                is_complete_ = false;
                return;
            }
            if (*b == '\x01') {
                prefix_end_ = b;
                break;
            }
            if (b - buffer_ > 11) {
                invalid();
                return;
            }
            ++b;
        }

        if (b + 1 >= buffer_end_) {
            is_complete_ = false;
            return;
        }
        // Spec: BeginString must be followed by BodyLength (tag 9).
        if (b[1] != '9') {
            invalid();
            return;
        }
        b += 3;  // past "\x01 9="

        size_t bodylength(0);

        while (true) {
            if (b >= buffer_end_) {
                is_complete_ = false;
                return;
            }
            if (*b == '\x01')
                break;
            if (*b < '0' || *b > '9') {
                invalid();
                return;
            }
            bodylength *= 10;
            bodylength += *b++ - '0';
        }

        ++b;
        if (b + 3 >= buffer_end_) {
            is_complete_ = false;
            return;
        }

        // Spec: BodyLength must be followed by MsgType (tag 35).
        if (*b != '3' || b[1] != '5') {
            invalid();
            return;
        }

        // Reject bodylength large enough to overflow pointer arithmetic on `b + bodylength`.
        // If bodylength would push past buffer_end_, the frame is incomplete (or truncated).
        if (bodylength > static_cast<std::size_t>(buffer_end_ - b)) {
            is_complete_ = false;
            return;
        }

        char const* checksum = b + bodylength;

        if (checksum + 7 > buffer_end_) {
            is_complete_ = false;
            return;
        }

        // SOH before checksum bounds iteration against a malformed message.
        if (*(checksum - 1) != '\x01') {
            invalid();
            return;
        }

        if (*(checksum + 6) != '\x01') {
            invalid();
            return;
        }

        begin_.buffer_ = b;
        begin_.current_.tag_ = 35;
        b += 3;
        begin_.current_.value_.begin_ = b;
        char const* msgtype_end = ::hffix::details::find_soh(b, checksum);
        if (msgtype_end >= checksum) {
            invalid();
            return;
        }
        begin_.current_.value_.end_ = msgtype_end;
        b = msgtype_end;

        end_.buffer_ = checksum;
        end_.current_.tag_ = 10;
        end_.current_.value_.begin_ = checksum + 3;
        end_.current_.value_.end_ = checksum + 6;

        char const* message_end = checksum + 7;
        begin_.message_end_ = message_end;
        end_.message_end_ = message_end;

        is_complete_ = true;
    }

    const_iterator end_;
    char const* buffer_;
    char const* buffer_end_;
    bool is_complete_;
    bool is_valid_;
    const_iterator begin_;
    char const* prefix_end_;

    void invalid() {
        is_complete_ = true;  // lets next_message_reader() resync past this frame
        is_valid_ = false;
    }
};

/**
 * \brief One entry of a FIX repeating group. Half-open iterator range over
 * the entry's fields; use `find_with_hint`/`begin`/`end` or pass to
 * `hffix::build_field_index`.
 */
class basic_group_entry {
public:
    using const_iterator = basic_message_reader_const_iterator;

    const_iterator begin() const noexcept { return begin_; }

    const_iterator end() const noexcept { return end_; }

    bool empty() const noexcept { return begin_ == end_; }

    [[nodiscard]] inline bool find_with_hint(int tag, const_iterator& it) const {
        return hffix::find_with_hint(begin_, end_, tag_equal(tag), it);
    }

    /** \brief Nested group lookup within this entry. */
    basic_group_view group(int count_tag, int first_tag_in_group) const;

    /**
     * \brief Compile-time dispatch via `groups::group_def<CountTag>`.
     * `static_assert` if unregistered.
     */
    template <int CountTag>
    basic_group_view group() const;

private:
    friend class basic_group_iterator;
    friend class basic_group_view;

    basic_group_entry(const_iterator b, const_iterator e) noexcept : begin_(b), end_(e) {}

    const_iterator begin_;
    const_iterator end_;
};

/**
 * \brief Forward iterator over `basic_group_entry` values in a group.
 */
class basic_group_iterator {
public:
    using const_iterator = basic_message_reader_const_iterator;
    using value_type = basic_group_entry;
    using reference = value_type;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    value_type operator*() const noexcept { return {entry_begin_, entry_end_}; }

    basic_group_iterator& operator++() noexcept {
        if (remaining_ > 0)
            --remaining_;
        if (remaining_ == 0 || entry_end_ == group_end_) {
            entry_begin_ = group_end_;
            entry_end_ = group_end_;
            remaining_ = 0;
        } else {
            entry_begin_ = entry_end_;
            compute_entry_end();
        }
        return *this;
    }

    bool operator==(basic_group_iterator const& o) const noexcept {
        return entry_begin_ == o.entry_begin_ && remaining_ == o.remaining_;
    }

    bool operator!=(basic_group_iterator const& o) const noexcept { return !(*this == o); }

private:
    friend class basic_group_view;

    basic_group_iterator(const_iterator entry_begin,
                         const_iterator group_end,
                         int delimiter,
                         std::size_t remaining) noexcept
        : entry_begin_(entry_begin),
          entry_end_(entry_begin),
          group_end_(group_end),
          delimiter_(delimiter),
          remaining_(remaining) {
        if (remaining_ == 0 || entry_begin_ == group_end_) {
            entry_begin_ = group_end_;
            entry_end_ = group_end_;
            remaining_ = 0;
        } else {
            compute_entry_end();
        }
    }

    void compute_entry_end() noexcept {
        entry_end_ = entry_begin_;
        ++entry_end_;
        while (entry_end_ != group_end_ && entry_end_->tag() != delimiter_) {
            ++entry_end_;
        }
    }

    const_iterator entry_begin_;
    const_iterator entry_end_;
    const_iterator group_end_;
    int delimiter_;
    std::size_t remaining_;
};

/**
 * \brief View over a FIX repeating group. Iterates `basic_group_entry`.
 * `size()` is the declared NoXxx count; actual iteration may be shorter
 * if the message is truncated.
 */
class basic_group_view {
public:
    using iterator = basic_group_iterator;
    using const_iterator = iterator;

    iterator begin() const noexcept {
        auto it = view_begin_;
        while (it != view_end_ && it->tag() != delimiter_)
            ++it;
        return iterator(it, view_end_, delimiter_, count_);
    }

    iterator end() const noexcept { return iterator(view_end_, view_end_, delimiter_, 0); }

    std::size_t size() const noexcept { return count_; }

    bool empty() const noexcept { return count_ == 0; }

private:
    friend class basic_message_reader;
    friend class basic_group_entry;

    using const_msg_iterator = basic_message_reader_const_iterator;

    basic_group_view(const_msg_iterator view_begin,
                     const_msg_iterator view_end,
                     std::size_t count,
                     int delimiter) noexcept
        : view_begin_(view_begin), view_end_(view_end), count_(count), delimiter_(delimiter) {}

    static basic_group_view empty_view(const_msg_iterator end_it, int delimiter) noexcept {
        return basic_group_view(end_it, end_it, 0, delimiter);
    }

    const_msg_iterator view_begin_;
    const_msg_iterator view_end_;
    std::size_t count_;
    int delimiter_;
};

inline basic_group_view basic_message_reader::group(int count_tag, int first_tag_in_group) const {
    auto it = begin();
    if (!hffix::find_with_hint(begin(), end(), tag_equal(count_tag), it)) {
        return basic_group_view::empty_view(end(), first_tag_in_group);
    }
    std::size_t count;
    if (!it->value().try_as_int<std::size_t>(count)) {
        return basic_group_view::empty_view(end(), first_tag_in_group);
    }
    ++it;
    return basic_group_view(it, end(), count, first_tag_in_group);
}

inline basic_group_view basic_group_entry::group(int count_tag, int first_tag_in_group) const {
    auto it = begin_;
    if (!hffix::find_with_hint(begin_, end_, tag_equal(count_tag), it)) {
        return basic_group_view::empty_view(end_, first_tag_in_group);
    }
    std::size_t count;
    if (!it->value().try_as_int<std::size_t>(count)) {
        return basic_group_view::empty_view(end_, first_tag_in_group);
    }
    ++it;
    return basic_group_view(it, end_, count, first_tag_in_group);
}

template <int CountTag>
inline basic_group_view basic_message_reader::group() const {
    static_assert(groups::group_def<CountTag>::first_tag != 0,
                  "Unknown CountTag for compile-time group dispatch. "
                  "Register with HFFIX_REGISTER_GROUP(NoXxx, FirstTag) "
                  "at namespace scope, or use the runtime overload "
                  "group(count_tag, first_tag_in_group).");
    return this->group(CountTag, groups::group_def<CountTag>::first_tag);
}

template <int CountTag>
inline basic_group_view basic_group_entry::group() const {
    static_assert(groups::group_def<CountTag>::first_tag != 0,
                  "Unknown CountTag for compile-time group dispatch. "
                  "Register with HFFIX_REGISTER_GROUP(NoXxx, FirstTag) "
                  "at namespace scope, or use the runtime overload "
                  "group(count_tag, first_tag_in_group).");
    return this->group(CountTag, groups::group_def<CountTag>::first_tag);
}

using group_entry = basic_group_entry;
using group_iterator = basic_group_iterator;
using group_view = basic_group_view;

/** @cond EXCLUDE */
namespace details {
bool is_tag_a_data_length(int tag);
}

/** @endcond */

HFFIX_ALWAYS_INLINE void basic_message_reader_const_iterator::increment() {
    // Mirror basic_message_reader::end_ so `it != r.end()` terminates.
    auto const set_at_end = [this]() noexcept {
        buffer_ = message_end_ - 7;
        current_.tag_ = 10;
        current_.value_.begin_ = message_end_ - 4;
        current_.value_.end_ = message_end_ - 1;
    };

    char const* p = current_.value_.end_ + 1;
    buffer_ = p;

    unsigned utag = 0;
    while (p < message_end_ && *p != '=' && *p != '\x01') {
        utag = utag * 10u + static_cast<unsigned>(*p - '0');
        ++p;
    }
    int tag = static_cast<int>(utag);
    if (p >= message_end_) [[unlikely]] {
        set_at_end();
        return;
    }

    if (*p == '\x01') [[unlikely]] {
        current_.tag_ = tag;
        current_.value_.begin_ = p;
        current_.value_.end_ = p;
        return;
    }

    ++p;
    char const* value_begin = p;
    char const* value_end = ::hffix::details::find_soh(p, message_end_);

    if (details::is_tag_a_data_length(tag)) [[unlikely]] {
        std::size_t data_len = details::atou<std::size_t>(value_begin, value_end);

        p = value_end + 1;
        buffer_ = p;
        unsigned unext_tag = 0;
        while (p < message_end_ && *p != '=') {
            unext_tag = unext_tag * 10u + static_cast<unsigned>(*p - '0');
            ++p;
        }
        int next_tag = static_cast<int>(unext_tag);
        if (p >= message_end_) [[unlikely]] {
            set_at_end();
            return;
        }
        ++p;
        if (data_len > static_cast<std::size_t>(message_end_ - p)) [[unlikely]] {
            set_at_end();
            return;
        }

        current_.tag_ = next_tag;
        current_.value_.begin_ = p;
        current_.value_.end_ = p + data_len;
        return;
    }

    current_.tag_ = tag;
    current_.value_.begin_ = value_begin;
    current_.value_.end_ = value_end;
}

/* @cond EXCLUDE */

namespace details {

struct length_tag_index {
    static constexpr int kBitmapSize = 1024;
    std::uint64_t bitmap[kBitmapSize / 64]{};
    int const* overflow_begin{};
    int const* overflow_end{};

    constexpr length_tag_index() {
        constexpr auto n = sizeof(length_fields) / sizeof(length_fields[0]);
        static_assert(
            std::is_sorted(std::begin(length_fields), std::end(length_fields)),
            "length_fields must be sorted ascending; bitmap/binary-search split depends on it.");
        std::size_t split = 0;
        while (split < n && length_fields[split] < kBitmapSize)
            ++split;
        for (std::size_t i = 0; i < split; ++i) {
            int const t = length_fields[i];
            bitmap[t / 64] |= std::uint64_t(1) << (t % 64);
        }
        overflow_begin = length_fields + split;
        overflow_end = length_fields + n;
    }

    HFFIX_ALWAYS_INLINE constexpr bool contains(int tag) const {
        if (tag >= 0 && tag < kBitmapSize) [[likely]] {
            return (bitmap[tag / 64] >> (tag % 64)) & 1u;
        }
        return std::binary_search(overflow_begin, overflow_end, tag);
    }
};

inline constinit length_tag_index const g_length_tag_table{};

HFFIX_ALWAYS_INLINE bool is_tag_a_data_length(int tag) {
    return g_length_tag_table.contains(tag);
}

template <typename AssociativeContainer>
struct field_name_streamer {
    int tag;
    AssociativeContainer const& field_dictionary;
    bool number_alternative;

    field_name_streamer(int tag, AssociativeContainer const& field_dictionary, bool number_alternative)
        : tag(tag), field_dictionary(field_dictionary), number_alternative(number_alternative) {}

    friend std::ostream& operator<<(std::ostream& os, field_name_streamer that) {
        typename AssociativeContainer::const_iterator i = that.field_dictionary.find(that.tag);
        if (i != that.field_dictionary.end())
            os << i->second;
        else if (that.number_alternative)
            os << that.tag;
        return os;
    }
};

}  // namespace details

/* @endcond */

/**
  * \brief Given a field tag number and a field name dictionary, returns a type which provides `operator<<`  to write the name of the field to an `std::ostream`.
  * \tparam AssociativeContainer The type of the field name dictionary. Must satisfy concept `AssociativeContainer<int, std::string>`, for example `std::map<int, std::string>` or `std::unordered_map<int, std::string>`. See https://en.cppreference.com/w/cpp/named_req/AssociativeContainer
  *
  * \param tag The field number.
  * \param field_dictionary The field dictionary.
  * \param or_number If true, prints the tag number when the dictionary has no mapping; if false, prints nothing. Default true.
  *
  * Example usage:
  * \code
  * std::map<int, std::string> dictionary;
  * hffix::dictionary_init_field(dictionary);
  * std::cout << hffix::field_name(hffix::tag::SenderCompID, dictionary) << '\n'; // Will print "SenderCompID\n".
  * std::cout << hffix::field_name(1000000, dictionary) << '\n';                  // Unknown field tag, will print "1000000\n".
  * std::cout << hffix::field_name(1000000, dictionary, false) << '\n';           // Unknown field tag, will print "\n".
  * \endcode
*/
template <typename AssociativeContainer>
details::field_name_streamer<AssociativeContainer> field_name(
    int tag, AssociativeContainer const& field_dictionary, bool or_number = true) {
    return details::field_name_streamer<AssociativeContainer>(tag, field_dictionary, or_number);
}

/**
 * \brief Random-access view of a parsed FIX message, built by
 * `build_field_index` into a caller-supplied `field_index_buffer<N>`.
 *
 * Parallel insertion-order arrays: `tags[i]` and packed
 * `pos_len[i] = (uint32_t pos << 32) | uint32_t len`, offsets relative
 * to `message_begin()`. A non-`authoritative()` index can miss fields
 * past slot `N` or skip indexing entirely when the message exceeds
 * `kMaxIndexableMessageBytes`.
 */
class basic_indexed_message {
public:
    static constexpr std::size_t kMaxIndexableMessageBytes =
        std::numeric_limits<std::uint32_t>::max();

    basic_indexed_message() = default;
    basic_indexed_message(basic_indexed_message const&) = default;
    basic_indexed_message& operator=(basic_indexed_message const&) = default;

    char const* message_begin() const { return msg_begin_; }

    [[nodiscard]] std::size_t field_count() const { return tags_.size(); }

    [[nodiscard]] bool truncated() const { return truncated_; }

    [[nodiscard]] bool overflowed() const { return overflowed_; }

    /**
     * `!truncated() && !overflowed()`. When false, `find_with_hint()` may report
     * absence for a field that exists past the index.
     */
    [[nodiscard]] bool authoritative() const { return !truncated_ && !overflowed_; }

    int tag_at(std::size_t i) const { return tags_[i]; }

    field_value value_at(std::size_t i) const {
        std::uint64_t const pl = pos_len_[i];
        field_value v;
        v.begin_ = msg_begin_ + (pl >> 32);
        v.end_ = v.begin_ + static_cast<std::uint32_t>(pl);
        return v;
    }

    /**
     * \brief Find a field by tag using a position hint.
     *
     * Scans `[hint, field_count())` then wraps to `[0, hint)`. On hit,
     * `hint` is set to the found index; on miss, unmodified.
     *
     * `hint == 0` returns the first occurrence. `hint > 0` returns the
     * nearest occurrence at-or-after `hint`, not the first one — when
     * the tag appears multiple times (e.g. across repeating-group
     * entries) the result depends on `hint`. Pass `hint = 0` for
     * first-occurrence semantics; use `has(tag)` for presence-only.
     *
     * \param tag Field tag to find.
     * \param hint In: scan start. Out: found index on hit; unmodified on miss.
     * \return Field value, or default-constructed `field_value` if not found.
     */
    inline field_value find_with_hint(int tag, std::size_t& hint) const {
        std::size_t const n = tags_.size();
        if (hint > n)
            hint = 0;
        std::size_t const suffix_n = n - hint;
        std::size_t const i = ::hffix::details::find_tag_in_index(tags_.data() + hint, suffix_n, tag);
        if (i != suffix_n) {
            hint += i;
            return value_at(hint);
        }
        std::size_t const j = ::hffix::details::find_tag_in_index(tags_.data(), hint, tag);
        if (j != hint) {
            hint = j;
            return value_at(hint);
        }
        return {};
    }

    [[nodiscard]] bool has(int tag) const {
        return ::hffix::details::find_tag_in_index(tags_.data(), tags_.size(), tag) != tags_.size();
    }

private:
    template <std::size_t N>
    friend basic_indexed_message build_field_index(basic_message_reader const&,
                                                   field_index_buffer<N>&);
    template <std::size_t N>
    friend basic_indexed_message build_field_index(basic_group_entry const&, field_index_buffer<N>&);

    char const* msg_begin_ = nullptr;
    std::span<int const> tags_;
    std::span<std::uint64_t const> pos_len_;
    bool truncated_ = false;
    bool overflowed_ = false;
};

using indexed_message = basic_indexed_message;

template <std::size_t N>
basic_indexed_message build_field_index(basic_message_reader const& r,
                                        field_index_buffer<N>& idx_buffer) {
    basic_indexed_message out;
    if (!r.is_valid())
        return out;
    out.msg_begin_ = r.message_begin();

    std::size_t const msg_size = r.message_size();
    if (msg_size > basic_indexed_message::kMaxIndexableMessageBytes) [[unlikely]] {
        out.overflowed_ = true;
        return out;
    }

    std::size_t count = 0;
    auto it = r.begin();
    auto end = r.end();
    for (; it != end && count < N; ++it) {
        auto const& v = it->value();
        std::ptrdiff_t const pos = v.begin() - out.msg_begin_;
        std::ptrdiff_t const len = v.end() - v.begin();
        idx_buffer.tags[count] = it->tag();
        idx_buffer.pos_len[count] = (static_cast<std::uint64_t>(pos) << 32) |
                                    static_cast<std::uint64_t>(static_cast<std::uint32_t>(len));
        ++count;
    }
    out.truncated_ = (it != end);
    out.tags_ = std::span<int const>(idx_buffer.tags, count);
    out.pos_len_ = std::span<std::uint64_t const>(idx_buffer.pos_len, count);
    return out;
}

/** Group-entry overload. Offsets are packed against the entry's first byte. */
template <std::size_t N>
basic_indexed_message build_field_index(basic_group_entry const& entry,
                                        field_index_buffer<N>& idx_buffer) {
    basic_indexed_message out;
    auto it = entry.begin();
    auto end = entry.end();
    if (it == end)
        return out;
    out.msg_begin_ = it.buffer_begin();

    constexpr std::uint64_t kMax32 = std::numeric_limits<std::uint32_t>::max();
    std::size_t count = 0;
    for (; it != end && count < N; ++it) {
        auto const& v = it->value();
        std::ptrdiff_t const pos = v.begin() - out.msg_begin_;
        std::ptrdiff_t const len = v.end() - v.begin();
        if (pos < 0 || static_cast<std::uint64_t>(pos) > kMax32 || len < 0 ||
            static_cast<std::uint64_t>(len) > kMax32) [[unlikely]] {
            out.overflowed_ = true;
            return out;
        }
        idx_buffer.tags[count] = it->tag();
        idx_buffer.pos_len[count] = (static_cast<std::uint64_t>(pos) << 32) |
                                    static_cast<std::uint64_t>(static_cast<std::uint32_t>(len));
        ++count;
    }
    out.truncated_ = (it != end);
    out.tags_ = std::span<int const>(idx_buffer.tags, count);
    out.pos_len_ = std::span<std::uint64_t const>(idx_buffer.pos_len, count);
    return out;
}

using message_reader = basic_message_reader;
using message_reader_const_iterator = basic_message_reader_const_iterator;

/**
 * \brief Range over consecutive FIX messages packed into a single buffer.
 *
 * Iterates contiguous messages produced by typical FIX wire delivery
 * (e.g. one `recv()` returning N concatenated frames). Yields only
 * `is_complete() && is_valid()` readers. Invalid frames between valid
 * ones are skipped via `next_message_reader()` resync. Iteration stops
 * at the first incomplete frame; the position of that frame is
 * available from `iterator::remainder()` for callers that buffer the
 * tail across reads.
 *
 * Thread-safety: the range itself holds two pointers and is trivially
 * copyable. Iterators hold a `basic_message_reader` by value and are
 * independent across copies.
 */
class basic_message_range {
public:
    using value_type = basic_message_reader;

    explicit basic_message_range(std::span<char const> buf) noexcept
        : begin_(buf.data()), end_(buf.data() + buf.size()) {}

    basic_message_range(char const* begin, char const* end) noexcept : begin_(begin), end_(end) {}

    basic_message_range(char const* buf, std::size_t n) noexcept : begin_(buf), end_(buf + n) {}

    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = basic_message_reader;
        using reference = basic_message_reader const&;
        using pointer = basic_message_reader const*;
        using difference_type = std::ptrdiff_t;

        iterator(char const* b, char const* e) noexcept : r_(b, e) { skip_invalid(); }

        reference operator*() const noexcept { return r_; }

        pointer operator->() const noexcept { return &r_; }

        iterator& operator++() noexcept {
            HFFIX_ASSERT(r_.is_complete(), "basic_message_range::iterator: ++ past end of range.");
            r_ = r_.next_message_reader();
            skip_invalid();
            return *this;
        }

        iterator operator++(int) noexcept {
            iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        friend bool operator==(iterator const& a, iterator const& b) noexcept {
            bool const a_end = !a.r_.is_complete();
            bool const b_end = !b.r_.is_complete();
            if (a_end && b_end)
                return true;
            if (a_end != b_end)
                return false;
            return a.r_.buffer_begin() == b.r_.buffer_begin();
        }

        friend bool operator!=(iterator const& a, iterator const& b) noexcept { return !(a == b); }

        /**
         * \brief Buffer position where iteration stopped.
         *
         * On the past-the-end iterator (`rng.end()` or one advanced
         * past the final complete frame), this points at the first
         * byte of the unconsumed tail (an incomplete frame, or the
         * buffer end). Callers using stream re-feeding copy
         * `[remainder(), buffer_end)` into the next read buffer.
         */
        char const* remainder() const noexcept { return r_.buffer_begin(); }

    private:
        void skip_invalid() noexcept {
            while (r_.is_complete() && !r_.is_valid()) {
                r_ = r_.next_message_reader();
            }
        }

        basic_message_reader r_;
    };

    iterator begin() const noexcept { return iterator(begin_, end_); }

    iterator end() const noexcept { return iterator(end_, end_); }

private:
    char const* begin_;
    char const* end_;
};

using message_range = basic_message_range;

/**
 * \brief Construct a range over consecutive FIX messages in `buf`.
 */
inline basic_message_range messages(std::span<char const> buf) noexcept {
    return basic_message_range(buf);
}

inline basic_message_range messages(char const* begin, char const* end) noexcept {
    return basic_message_range(begin, end);
}

inline basic_message_range messages(char const* buf, std::size_t n) noexcept {
    return basic_message_range(buf, n);
}

/**
 * \brief Callback-style batched parse. Invokes `fn` once per complete,
 * valid FIX message in `buf`.
 *
 * Equivalent to a loop over `messages(buf)` but threads the
 * tail-position return through the call, which is the common shape in
 * a feed-handler dispatch loop. Invalid frames are skipped via
 * resync; iteration stops at the first incomplete frame.
 *
 * \param buf Span over the byte range to scan.
 * \param fn Callable invoked as `fn(basic_message_reader const&)`
 *   for each yielded message.
 * \return Pointer to the first byte of the unconsumed tail. Equals
 *   `buf.data() + buf.size()` when the buffer drained cleanly,
 *   otherwise points at an incomplete frame the caller should keep
 *   for the next read.
 */
template <class Fn>
HFFIX_HOT char const* for_each_message(std::span<char const> buf, Fn&& fn) {
    basic_message_reader r(buf);
    while (r.is_complete()) {
        if (r.is_valid()) {
            HFFIX_PREFETCH(r.message_end());
            fn(static_cast<basic_message_reader const&>(r));
        }
        r = r.next_message_reader();
    }
    return r.buffer_begin();
}

template <class Fn>
HFFIX_HOT char const* for_each_message(char const* begin, char const* end, Fn&& fn) {
    return for_each_message(std::span<char const>(begin, static_cast<std::size_t>(end - begin)),
                            std::forward<Fn>(fn));
}

}  // namespace hffix

#include <hffix_groups.hpp>
