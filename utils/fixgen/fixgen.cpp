// Write N synthetic FIX 5.0 SP2 messages over FIXT 1.1 to <path>.
//   fixgen -o <path> [-n count] [-s seed]
// Output is wire-format (no newlines); pipe through fixprint to inspect.

#include <hffix.hpp>
#include <hffix_fields.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <string_view>

namespace {

constexpr std::size_t kBuf = 1 << 18;

char const* const kSymbols[] = {
    "AAPL",   "MSFT",   "GOOG",   "GOOGL",  "AMZN",   "TSLA",   "META",   "NVDA",   "BRK-B",
    "V",      "JPM",    "JNJ",    "WMT",    "MA",     "PG",     "XOM",    "UNH",    "HD",
    "BAC",    "ABBV",   "PFE",    "AVGO",   "CVX",    "KO",     "LLY",    "MRK",    "COST",
    "PEP",    "TMO",    "ADBE",   "CSCO",   "DIS",    "NKE",    "ABT",    "ACN",    "MCD",
    "TXN",    "DHR",    "CRM",    "NEE",    "VZ",     "ORCL",   "NFLX",   "AMD",    "INTC",
    "AMGN",   "IBM",    "T",      "BMY",    "QCOM",   "GE",     "GS",     "MS",     "AXP",
    "CAT",    "BLK",    "RTX",    "SPGI",   "BKNG",   "AMT",    "NOW",    "INTU",   "DE",
    "GILD",   "ISRG",   "MDT",    "ELV",    "ADP",    "LMT",    "MMC",    "SYK",    "CB",
    "MO",     "SCHW",   "CI",     "ZTS",    "BDX",    "USB",    "PNC",    "TJX",    "REGN",
    "BSX",    "EOG",    "EQIX",   "SHW",    "AMAT",   "SO",     "NSC",    "PLD",    "NOC",
    "CSX",    "FCX",    "ADI",    "ITW",    "FDX",    "KDP",    "ICE",    "GD",     "EW",
    "EMR",    "COP",    "MMM",    "BX",     "MDLZ",   "MU",     "PYPL",   "PANW",   "LRCX",
    "KLAC",   "SBUX",   "SPY",    "QQQ",    "IWM",    "DIA",    "VOO",    "VTI",    "EFA",
    "EEM",    "AGG",    "BND",    "GLD",    "SLV",    "USO",    "UNG",    "TLT",    "IEF",
    "SHY",    "LQD",    "HYG",    "VEA",    "VWO",    "IVV",    "IJR",    "IJH",    "IAU",
    "GDX",    "GDXJ",   "XLF",    "XLE",    "XLK",    "XLV",    "XLI",    "XLP",    "XLY",
    "XLU",    "XLB",    "XLRE",   "XLC",    "VNQ",    "DBC",    "ESH5",   "ESM5",   "ESU5",
    "ESZ5",   "NQH5",   "NQM5",   "NQU5",   "NQZ5",   "YMH5",   "YMM5",   "RTYH5",  "RTYM5",
    "CLG5",   "CLH5",   "NGG5",   "NGH5",   "RBG5",   "HOG5",   "BZG5",   "GCG5",   "GCJ5",
    "GCM5",   "SIH5",   "SIK5",   "HGH5",   "HGK5",   "PAH5",   "PLF5",   "ZBH5",   "ZNH5",
    "ZFH5",   "ZTH5",   "ZCH5",   "ZSH5",   "ZWH5",   "ZLH5",   "ZMH5",   "VXG5",   "VXH5",
    "VXM5",   "EURUSD", "GBPUSD", "USDJPY", "AUDUSD", "USDCAD", "USDCHF", "NZDUSD", "USDCNH",
    "EURGBP", "EURJPY", "GBPJPY", "AUDJPY", "EURCHF", "EURAUD", "EURNZD", "EURCAD", "EURSEK",
    "EURNOK", "USDMXN", "USDZAR", "6EH5",   "6JH5",   "6BH5",   "6AH5",   "6CH5",   "6SH5",
    "BTCH5",  "BTCM5",  "ETHH5",  "ETHM5",
};
constexpr std::size_t kNumSymbols = sizeof(kSymbols) / sizeof(kSymbols[0]);

char const* const kSenders[] = {"A", "B", "CME", "CLNT", "EXCH"};
constexpr std::size_t kNumSenders = sizeof(kSenders) / sizeof(kSenders[0]);

// Random timestamp on 2024-01-15; hour/min/sec/microsecond digits all vary
// so the SWAR parser is exercised across every position. Determinism via seeded rng.
template <typename Rng>
auto base_time(Rng& rng) noexcept {
    using namespace std::chrono;
    std::uniform_int_distribution<int> hour_dist(9, 15);
    std::uniform_int_distribution<int> minute_dist(0, 59);
    std::uniform_int_distribution<int> second_dist(0, 59);
    std::uniform_int_distribution<int> micro_dist(0, 999999);
    return sys_days{year{2024} / January / 15} + hours{hour_dist(rng)} + minutes{minute_dist(rng)} +
           seconds{second_dist(rng)} + microseconds{micro_dist(rng)};
}

// 20-char ClOrdID: "CL" + 8 hex of seq + 10 hex of (seq * 0x9E3779B97F4A7C15 mixer).
std::string_view make_clordid(int seq) noexcept {
    static thread_local char buf[20];
    static constexpr char hex[] = "0123456789ABCDEF";
    buf[0] = 'C';
    buf[1] = 'L';
    auto u = static_cast<std::uint32_t>(seq);
    for (int i = 0; i < 8; ++i)
        buf[2 + i] = hex[(u >> ((7 - i) * 4)) & 0xF];
    std::uint64_t mix = static_cast<std::uint64_t>(seq) * 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 10; ++i)
        buf[10 + i] = hex[(mix >> ((9 - i) * 4)) & 0xF];
    return std::string_view{buf, sizeof(buf)};
}

template <typename Rng>
[[nodiscard]] bool write_session_header(hffix::message_writer& w,
                                        char const* msg_type,
                                        int seq,
                                        char const* sender,
                                        char const* target,
                                        Rng& rng) {
    w.push_back_header("FIXT.1.1");
    w.push_back_string(hffix::tag::MsgType, std::string_view{msg_type});
    w.push_back_string(hffix::tag::SenderCompID, std::string_view{sender});
    w.push_back_string(hffix::tag::TargetCompID, std::string_view{target});
    w.push_back_int(hffix::tag::MsgSeqNum, seq);
    w.push_back_timestamp(hffix::tag::SendingTime, base_time(rng));
    return w.ok();
}

[[nodiscard]] bool push_back_trailer_msg(hffix::message_writer& w, char const* what, int seq) {
    if (!w.push_back_trailer()) {
        std::fprintf(stderr, "fixgen: writer failure on %s seq=%d\n", what, seq);
        return false;
    }
    return true;
}

template <typename Rng>
std::size_t write_heartbeat(
    char* buf, std::size_t cap, int seq, char const* sender, char const* target, Rng& rng, bool& ok) {
    hffix::message_writer w(buf, buf + cap);
    if (!write_session_header(w, "0", seq, sender, target, rng)) {
        ok = false;
        return 0;
    }
    if (!push_back_trailer_msg(w, "Heartbeat", seq)) {
        ok = false;
        return 0;
    }
    return static_cast<std::size_t>(w.message_end() - buf);
}

template <typename Rng>
std::size_t write_logon(
    char* buf, std::size_t cap, int seq, char const* sender, char const* target, Rng& rng, bool& ok) {
    hffix::message_writer w(buf, buf + cap);
    if (!write_session_header(w, "A", seq, sender, target, rng)) {
        ok = false;
        return 0;
    }
    w.push_back_int(hffix::tag::EncryptMethod, 0);
    w.push_back_int(hffix::tag::HeartBtInt, 30);
    w.push_back_int(hffix::tag::DefaultApplVerID, 9);
    if (!push_back_trailer_msg(w, "Logon", seq)) {
        ok = false;
        return 0;
    }
    return static_cast<std::size_t>(w.message_end() - buf);
}

template <typename Rng>
std::size_t write_logout(
    char* buf, std::size_t cap, int seq, char const* sender, char const* target, Rng& rng, bool& ok) {
    hffix::message_writer w(buf, buf + cap);
    if (!write_session_header(w, "5", seq, sender, target, rng)) {
        ok = false;
        return 0;
    }
    if (!push_back_trailer_msg(w, "Logout", seq)) {
        ok = false;
        return 0;
    }
    return static_cast<std::size_t>(w.message_end() - buf);
}

template <typename Rng>
std::size_t write_new_order(char* buf,
                            std::size_t cap,
                            int seq,
                            char const* sender,
                            char const* target,
                            char const* symbol,
                            int price,
                            int qty,
                            char side,
                            Rng& rng,
                            bool& ok) {
    hffix::message_writer w(buf, buf + cap);
    if (!write_session_header(w, "D", seq, sender, target, rng)) {
        ok = false;
        return 0;
    }
    w.push_back_string(hffix::tag::ClOrdID, make_clordid(seq));
    w.push_back_char(hffix::tag::HandlInst, '1');
    w.push_back_string(hffix::tag::Symbol, std::string_view{symbol});
    w.push_back_char(hffix::tag::Side, side);
    w.push_back_timestamp(hffix::tag::TransactTime, base_time(rng));
    w.push_back_int(hffix::tag::OrderQty, qty);
    w.push_back_char(hffix::tag::OrdType, '2');
    w.push_back_decimal(hffix::tag::Price, price, -2);
    w.push_back_char(hffix::tag::TimeInForce, '0');
    if (!push_back_trailer_msg(w, "NewOrderSingle", seq)) {
        ok = false;
        return 0;
    }
    return static_cast<std::size_t>(w.message_end() - buf);
}

template <typename Rng>
std::size_t write_exec_report(char* buf,
                              std::size_t cap,
                              int seq,
                              char const* sender,
                              char const* target,
                              char const* symbol,
                              int price,
                              int qty,
                              char side,
                              Rng& rng,
                              bool& ok) {
    hffix::message_writer w(buf, buf + cap);
    if (!write_session_header(w, "8", seq, sender, target, rng)) {
        ok = false;
        return 0;
    }
    w.push_back_int(hffix::tag::OrderID, seq);
    w.push_back_int(hffix::tag::ExecID, seq * 7);
    w.push_back_char(hffix::tag::ExecType, '2');
    w.push_back_char(hffix::tag::OrdStatus, '2');
    w.push_back_string(hffix::tag::Symbol, std::string_view{symbol});
    w.push_back_char(hffix::tag::Side, side);
    w.push_back_int(hffix::tag::LeavesQty, 0);
    w.push_back_int(hffix::tag::CumQty, qty);
    w.push_back_decimal(hffix::tag::AvgPx, price, -2);
    w.push_back_decimal(hffix::tag::LastPx, price, -2);
    w.push_back_int(hffix::tag::LastQty, qty);
    if (!push_back_trailer_msg(w, "ExecutionReport", seq)) {
        ok = false;
        return 0;
    }
    return static_cast<std::size_t>(w.message_end() - buf);
}

template <typename Rng>
std::size_t write_md_snapshot(
    char* buf, std::size_t cap, int seq, char const* sender, char const* target, Rng& rng, bool& ok) {
    std::uniform_int_distribution<int> levels_dist(3, 10);
    std::uniform_int_distribution<int> qty_dist(1, 5000);
    std::uniform_int_distribution<int> mid_dist(50000, 600000);
    std::uniform_int_distribution<int> spread_dist(5, 50);
    std::uniform_int_distribution<int> sym_dist(0, kNumSymbols - 1);

    char const* symbol = kSymbols[sym_dist(rng)];
    int n_bid = levels_dist(rng);
    int n_ask = levels_dist(rng);
    int mid = mid_dist(rng);
    int spread = spread_dist(rng);

    hffix::message_writer w(buf, buf + cap);
    if (!write_session_header(w, "W", seq, sender, target, rng)) {
        ok = false;
        return 0;
    }
    w.push_back_string(hffix::tag::Symbol, std::string_view{symbol});
    w.push_back_int(hffix::tag::NoMDEntries, n_bid + n_ask);
    for (int i = 0; i < n_bid; ++i) {
        w.push_back_char(hffix::tag::MDEntryType, '0');
        w.push_back_decimal(hffix::tag::MDEntryPx, mid - spread - i * 10, -2);
        w.push_back_int(hffix::tag::MDEntrySize, qty_dist(rng));
    }
    for (int i = 0; i < n_ask; ++i) {
        w.push_back_char(hffix::tag::MDEntryType, '1');
        w.push_back_decimal(hffix::tag::MDEntryPx, mid + spread + i * 10, -2);
        w.push_back_int(hffix::tag::MDEntrySize, qty_dist(rng));
    }
    if (!push_back_trailer_msg(w, "MarketDataSnapshot", seq)) {
        ok = false;
        return 0;
    }
    return static_cast<std::size_t>(w.message_end() - buf);
}

template <typename Rng>
std::size_t write_md_incremental(
    char* buf, std::size_t cap, int seq, char const* sender, char const* target, Rng& rng, bool& ok) {
    std::uniform_int_distribution<int> entries_dist(1, 200);
    std::uniform_int_distribution<int> qty_dist(1, 5000);
    std::uniform_int_distribution<int> px_dist(50000, 600000);
    char const update_actions[] = {'0', '1', '2'};

    int n = entries_dist(rng);

    int indices[kNumSymbols];
    for (std::size_t i = 0; i < kNumSymbols; ++i)
        indices[i] = static_cast<int>(i);
    for (int i = 0; i < n; ++i) {
        std::uniform_int_distribution<int> swap_dist(i, static_cast<int>(kNumSymbols) - 1);
        int j = swap_dist(rng);
        std::swap(indices[i], indices[j]);
    }

    hffix::message_writer w(buf, buf + cap);
    if (!write_session_header(w, "X", seq, sender, target, rng)) {
        ok = false;
        return 0;
    }
    w.push_back_int(hffix::tag::NoMDEntries, n);
    for (int i = 0; i < n; ++i) {
        w.push_back_char(hffix::tag::MDUpdateAction, update_actions[i % 3]);
        w.push_back_char(hffix::tag::MDEntryType, (i & 1) ? '0' : '1');
        w.push_back_string(hffix::tag::Symbol, std::string_view{kSymbols[indices[i]]});
        w.push_back_decimal(hffix::tag::MDEntryPx, px_dist(rng), -2);
        w.push_back_int(hffix::tag::MDEntrySize, qty_dist(rng));
    }
    if (!push_back_trailer_msg(w, "MarketDataIncremental", seq)) {
        ok = false;
        return 0;
    }
    return static_cast<std::size_t>(w.message_end() - buf);
}

struct Args {
    std::string out;
    long count = 200000;
    std::uint32_t seed = 42;
};

bool parse_args(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) {
            args.out = argv[++i];
        } else if (a == "-n" && i + 1 < argc) {
            args.count = std::atol(argv[++i]);
        } else if (a == "-s" && i + 1 < argc) {
            args.seed = static_cast<std::uint32_t>(std::atol(argv[++i]));
        } else if (a == "-h" || a == "--help") {
            return false;
        } else {
            std::cerr << "unknown argument: " << a << "\n";
            return false;
        }
    }
    if (args.out.empty()) {
        std::cerr << "missing -o <path>\n";
        return false;
    }
    if (args.count <= 0) {
        std::cerr << "count must be positive\n";
        return false;
    }
    return true;
}

void usage(char const* argv0) {
    std::cerr << "usage: " << argv0 << " -o <path> [-n count] [-s seed]\n";
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) {
        usage(argv[0]);
        return 2;
    }

    std::ostream* out = nullptr;
    std::ofstream file;
    if (args.out == "-") {
        out = &std::cout;
    } else {
        file.open(args.out, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::cerr << "cannot open " << args.out << " for writing\n";
            return 1;
        }
        out = &file;
    }

    std::mt19937 rng(args.seed);
    std::discrete_distribution<int> type_dist({
        2,   // Heartbeat
        25,  // NewOrderSingle
        25,  // ExecutionReport
        20,  // MarketDataSnapshot
        25,  // MarketDataIncremental
        1,   // Logon
        1,   // Logout
    });
    std::uniform_int_distribution<int> sym_dist(0, kNumSymbols - 1);
    std::uniform_int_distribution<int> sender_dist(0, kNumSenders - 1);
    std::uniform_int_distribution<int> price_dist(50000, 600000);
    std::uniform_int_distribution<int> qty_dist(1, 500);
    std::uniform_int_distribution<int> side_dist(0, 1);

    char buf[kBuf];
    std::size_t total_bytes = 0;
    std::size_t min_seen = SIZE_MAX;
    std::size_t max_seen = 0;
    bool dataset_ok = true;

    for (long i = 0; i < args.count; ++i) {
        int t = type_dist(rng);
        char const* sym = kSymbols[sym_dist(rng)];
        char const* sender = kSenders[sender_dist(rng)];
        char const* target = kSenders[sender_dist(rng)];
        int price = price_dist(rng);
        int qty = qty_dist(rng);
        char side = side_dist(rng) == 0 ? '1' : '2';
        int seq = static_cast<int>(i + 1);

        std::size_t n = 0;
        switch (t) {
            case 0:
                n = write_heartbeat(buf, kBuf, seq, sender, target, rng, dataset_ok);
                break;
            case 1:
                n = write_new_order(
                    buf, kBuf, seq, sender, target, sym, price, qty, side, rng, dataset_ok);
                break;
            case 2:
                n = write_exec_report(
                    buf, kBuf, seq, sender, target, sym, price, qty, side, rng, dataset_ok);
                break;
            case 3:
                n = write_md_snapshot(buf, kBuf, seq, sender, target, rng, dataset_ok);
                break;
            case 4:
                n = write_md_incremental(buf, kBuf, seq, sender, target, rng, dataset_ok);
                break;
            case 5:
                n = write_logon(buf, kBuf, seq, sender, target, rng, dataset_ok);
                break;
            case 6:
                n = write_logout(buf, kBuf, seq, sender, target, rng, dataset_ok);
                break;
        }
        if (!dataset_ok) {
            std::fprintf(stderr, "fixgen: aborting at seq=%d after writer failure\n", seq);
            return 1;
        }
        out->write(buf, static_cast<std::streamsize>(n));
        total_bytes += n;
        if (n < min_seen)
            min_seen = n;
        if (n > max_seen)
            max_seen = n;
    }

    std::cerr << "wrote " << args.count << " messages, " << total_bytes << " bytes to " << args.out
              << " (min " << min_seen << "B, max " << max_seen << "B, avg "
              << (total_bytes / static_cast<std::size_t>(args.count)) << "B)\n";
    return 0;
}
