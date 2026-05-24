// Uses only the API surface that jamesdbrock/hffix exposes

#include <benchmark/benchmark.h>

#include <hffix.hpp>
#include <hffix_fields.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef HFFIX_BENCH_DATA_DIR
#define HFFIX_BENCH_DATA_DIR ""
#endif

namespace {

constexpr std::size_t kBufSize = 1 << 13;

using SysTime = std::chrono::system_clock::time_point;

SysTime live_timestamp() {
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
    w.push_back_trailer();
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
    w.push_back_trailer();
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
                auto v = it->value().as_int<int>();
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

// Counters include the clock::now() probe (~20-30 ns on M4); same overhead
// in every variant, so relative deltas are comparable, absolute p* are not.
template <int const* Tags, std::size_t N>
void BM_Parse_TailLatency_Iter(benchmark::State& state, Dataset const* ds) {
    using clk = std::chrono::steady_clock;
    char const* begin = ds->data.data();
    char const* end = begin + ds->data.size();
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
            auto it = r.begin();
            for (std::size_t k = 0; k < N; ++k) {
                if (r.find_with_hint(Tags[k], it)) {
                    auto v = it->value();
                    benchmark::DoNotOptimize(v);
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

void register_benches() {
    benchmark::RegisterBenchmark("BM_WriteLogon", BM_WriteLogon);
    benchmark::RegisterBenchmark("BM_WriteNewOrder", BM_WriteNewOrder);
    benchmark::RegisterBenchmark("BM_ReadMessageScan", BM_ReadMessageScan);
    benchmark::RegisterBenchmark("BM_ReadMessageFindFields", BM_ReadMessageFindFields);
    benchmark::RegisterBenchmark("BM_RoundTrip", BM_RoundTrip);

    auto const& datasets = load_datasets();
    if (datasets.empty()) {
        benchmark::RegisterBenchmark("BM_Parse/<no-dataset>", [](benchmark::State& s) {
            s.SkipWithError("no dataset; run cmake --build <build> --target bench_data");
        });
        return;
    }
    for (auto const& ds : datasets) {
        benchmark::RegisterBenchmark(
            ("BM_Parse_ScanMessages/" + ds.name).c_str(), BM_Parse_ScanMessages, &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_IterAllFields/" + ds.name).c_str(), BM_Parse_IterAllFields, &ds);
        benchmark::RegisterBenchmark(("BM_Parse_FindCommonFields/" + ds.name).c_str(),
                                     BM_Parse_FindTags<kCommonTags, std::size(kCommonTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(("BM_Parse_FindManyFields_Iter/" + ds.name).c_str(),
                                     BM_Parse_FindTags<kManyTags, std::size(kManyTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(("BM_Parse_FindLotsFields_Iter/" + ds.name).c_str(),
                                     BM_Parse_FindTags<kWideTags, std::size(kWideTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(("BM_Parse_Sequential_Iter/" + ds.name).c_str(),
                                     BM_Parse_FindTags<kSeqOrderTags, std::size(kSeqOrderTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(("BM_Parse_Random_Iter/" + ds.name).c_str(),
                                     BM_Parse_FindTags<kRandOrderTags, std::size(kRandOrderTags)>,
                                     &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_TailLatency_Sequential_Iter/" + ds.name).c_str(),
            BM_Parse_TailLatency_Iter<kSeqOrderTags, std::size(kSeqOrderTags)>,
            &ds);
        benchmark::RegisterBenchmark(
            ("BM_Parse_TailLatency_Random_Iter/" + ds.name).c_str(),
            BM_Parse_TailLatency_Iter<kRandOrderTags, std::size(kRandOrderTags)>,
            &ds);
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
