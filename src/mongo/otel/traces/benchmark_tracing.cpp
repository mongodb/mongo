// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/benchmark_tracing.h"

#include "mongo/config.h"

#ifdef MONGO_CONFIG_OTEL
#include "mongo/otel/traces/sampler/sampler.h"
#include "mongo/otel/traces/sampler/sampling_config.h"
#include "mongo/otel/traces/tracer_provider_service.h"
#include "mongo/otel/traces/tracer_provider_service_factory.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

#include <ctime>
#include <iostream>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/exporter.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>

// Renamed from the default "testing" to avoid colliding with gtest's namespace of the same name.
// This is opentelemetry-cpp's own test-only HTTP server, used here to stand in for a real OTLP
// collector so the real HTTP exporter can be benchmarked without a network dependency.
#define HTTP_SERVER_NS otel_benchmark_dummy_collector
#include <opentelemetry/ext/http/server/http_server.h>
#endif

namespace mongo::otel::traces {

#ifdef MONGO_CONFIG_OTEL
namespace {

/**
 * Wraps an exporter to count the spans it is asked to record and export, and the CPU time spent
 * exporting on the calling thread, which for a BatchSpanProcessor is its background thread.
 */
class CpuAccountingExporter : public opentelemetry::sdk::trace::SpanExporter {
public:
    struct Counters {
        Atomic<int64_t> made{0};
        Atomic<int64_t> exported{0};
        Atomic<int64_t> exportCpuNanos{0};
    };

    CpuAccountingExporter(std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> inner,
                          Counters* counters)
        : _inner(std::move(inner)), _counters(counters) {}

    std::unique_ptr<opentelemetry::sdk::trace::Recordable> MakeRecordable() noexcept override {
        _counters->made.fetchAndAdd(1);
        return _inner->MakeRecordable();
    }

    opentelemetry::sdk::common::ExportResult Export(
        const opentelemetry::nostd::span<std::unique_ptr<opentelemetry::sdk::trace::Recordable>>&
            spans) noexcept override {
        const auto start = threadCpuNanos();
        auto result = _inner->Export(spans);
        _counters->exportCpuNanos.fetchAndAdd(threadCpuNanos() - start);
        _counters->exported.fetchAndAdd(spans.size());
        return result;
    }

    bool ForceFlush(std::chrono::microseconds timeout) noexcept override {
        const auto start = threadCpuNanos();
        auto ok = _inner->ForceFlush(timeout);
        _counters->exportCpuNanos.fetchAndAdd(threadCpuNanos() - start);
        return ok;
    }

    bool Shutdown(std::chrono::microseconds timeout) noexcept override {
        return _inner->Shutdown(timeout);
    }

private:
    static int64_t threadCpuNanos() {
        timespec ts;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return int64_t{ts.tv_sec} * 1'000'000'000 + ts.tv_nsec;
    }

    std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> _inner;
    Counters* _counters;
};

opentelemetry::sdk::trace::BatchSpanProcessorOptions defaultBatchOptions() {
    opentelemetry::sdk::trace::BatchSpanProcessorOptions opts;
    opts.max_queue_size = 2048;
    opts.max_export_batch_size = 512;
    opts.schedule_delay_millis = std::chrono::milliseconds(1000);
    return opts;
}

/** Accepts and discards every request without inspecting it. */
class AcceptingHandler : public HTTP_SERVER_NS::HttpRequestCallback {
public:
    int onHttpRequest(const HTTP_SERVER_NS::HttpRequest&, HTTP_SERVER_NS::HttpResponse&) override {
        return 200;
    }
};

/**
 * HttpServer::addListeningPort() binds to every interface (SocketAddr(0, port), i.e. INADDR_ANY),
 * not just loopback. This instead binds to SocketAddr::Loopback, so the dummy collector below is
 * only reachable from this host.
 */
class LoopbackHttpServer : public HTTP_SERVER_NS::HttpServer {
public:
    int addLoopbackListeningPort() {
        SocketTools::Socket socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        socket.setNonBlocking();
        socket.setReuseAddr();
        SocketTools::SocketAddr addr(SocketTools::SocketAddr::Loopback, /*port=*/0);
        socket.bind(addr);
        socket.getsockname(addr);
        socket.listen(10);
        m_listeningSockets.push_back(socket);
        m_reactor.addSocket(socket, SocketTools::Reactor::Acceptable);
        return addr.port();
    }
};

/**
 * A minimal local OTLP collector, listening on loopback only, that immediately accepts and
 * discards every request. Stands in for a real collector so the real OtlpHttpExporter mongod uses
 * in production can be benchmarked without depending on an external service.
 */
class DummyCollector {
public:
    DummyCollector() {
        _port = _server.addLoopbackListeningPort();
        _server.addHandler("/", _handler);
        _server.start();
    }

    ~DummyCollector() {
        _server.stop();
    }

    DummyCollector(const DummyCollector&) = delete;
    DummyCollector& operator=(const DummyCollector&) = delete;

    std::string endpoint() const {
        return fmt::format("http://127.0.0.1:{}/v1/traces", _port);
    }

private:
    AcceptingHandler _handler;
    LoopbackHttpServer _server;
    int _port;
};

constexpr std::string_view kSamplingFlag = "featureFlagOtelTraceSampling";

}  // namespace

struct BenchmarkTracingSession::State {
    ScopedServerParameter samplingFlag{kSamplingFlag, "true"};
    std::unique_ptr<TracerProviderService> savedProviderService;
    SamplingParameters samplingParameters;
    bool usingRealExporter;
    std::unique_ptr<DummyCollector> dummyCollector;
    // Owned by the tracer provider.
    opentelemetry::sdk::trace::SpanProcessor* processor = nullptr;
    CpuAccountingExporter::Counters exportCounters;
};

BenchmarkTracingSession::BenchmarkTracingSession(SamplingParameters samplingParameters,
                                                 std::vector<std::string> sampledSpanNames) {
    _state = std::make_unique<State>();
    _state->samplingParameters = samplingParameters;
    _state->dummyCollector = std::make_unique<DummyCollector>();

    opentelemetry::exporter::otlp::OtlpHttpExporterOptions httpOpts;
    httpOpts.url = _state->dummyCollector->endpoint();
    std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> exporter =
        opentelemetry::exporter::otlp::OtlpHttpExporterFactory::Create(httpOpts);
    exporter =
        std::make_unique<CpuAccountingExporter>(std::move(exporter), &_state->exportCounters);
    std::unique_ptr<opentelemetry::sdk::trace::SpanProcessor> processor =
        opentelemetry::sdk::trace::BatchSpanProcessorFactory::Create(std::move(exporter),
                                                                     defaultBatchOptions());
    _state->processor = processor.get();
    auto service = createNoOpTracerProviderService();
    service->setTracerProvider_ForTest(
        opentelemetry::sdk::trace::TracerProviderFactory::Create(std::move(processor)));
    _state->savedProviderService = swapGlobalTracerProviderServiceForTest(std::move(service));

    if (samplingParameters.factor > 0.0) {
        StringMap<SamplingParameters> overrides;
        for (auto& name : sampledSpanNames) {
            overrides[name] = samplingParameters;
        }
        TracingSampler::get().updateInternalConfig(samplingParameters, overrides);
        if (samplingParameters.factor >= 1.0) {
            TracingSampler::get().updateExternalConfig(samplingParameters.rateLimits);
        }
    }
}

int64_t BenchmarkTracingSession::exportedSpans() const {
    _state->processor->ForceFlush();
    return _state->exportCounters.exported.load();
}

BenchmarkTracingSession::~BenchmarkTracingSession() {
    const auto exportedSpans = this->exportedSpans();
    {
        const auto& c = _state->exportCounters;
        const auto exported = c.exported.load();
        std::cerr << "[benchmark_tracing] spans made=" << c.made.load() << " exported=" << exported
                  << " dropped=" << c.made.load() - exported << " export-thread CPU/span="
                  << (exported ? c.exportCpuNanos.load() / exported : 0) << "ns" << std::endl;
    }
    const auto factor = _state->samplingParameters.factor;
    if (factor > 0.0) {
        TracingSampler::get().updateInternalConfig(SamplingParameters{}, {});
        TracingSampler::get().updateExternalConfig(RateLimitParams{});
    }
    swapGlobalTracerProviderServiceForTest(std::move(_state->savedProviderService));

    // A misconfigured benchmark would silently measure the wrong thing and pass any threshold that
    // compares it with an untraced run, so fail loudly instead.
    if (factor >= 1.0) {
        invariant(exportedSpans > 0, "Traced benchmark did not export any spans");
    } else if (factor == 0.0) {
        invariant(exportedSpans == 0, "Untraced benchmark unexpectedly exported spans");
    }
}

bool BenchmarkTracingSession::isSupported() {
    return true;
}

#else

struct BenchmarkTracingSession::State {};

BenchmarkTracingSession::BenchmarkTracingSession(SamplingParameters, std::vector<std::string>) {}

BenchmarkTracingSession::~BenchmarkTracingSession() = default;

bool BenchmarkTracingSession::isSupported() {
    return false;
}

int64_t BenchmarkTracingSession::exportedSpans() const {
    return 0;
}

#endif

}  // namespace mongo::otel::traces
