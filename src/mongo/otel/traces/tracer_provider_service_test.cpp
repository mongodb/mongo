// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/tracer_provider_service.h"

#include "mongo/otel/traces/tracer_provider_service_factory.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/exporter.h>
#include <opentelemetry/sdk/trace/span_data.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>

namespace mongo::otel::traces {
namespace {

constexpr auto kServiceName = "mongod";

/**
 * Exercises the concurrency contract between operation threads and shutdown. Operation threads read
 * the provider (as span.cpp's Span::_start does) while another thread shuts the service down. Under
 * TSAN this deterministically flags the data race on the non-atomic _tracerProvider member; without
 * a sanitizer it can crash (SIGBUS/SIGSEGV) when the provider is destroyed out from under a reader.
 */
TEST(TracerProviderServiceTest, ConcurrentGetTracerAndShutdownIsRaceFree) {
    unittest::TempDir dir("tracer_provider_service_test");
    auto swService = createFileTracerProviderService(kServiceName, dir.path());
    ASSERT_OK(swService);
    auto service = std::move(swService.getValue());

    constexpr int kReaders = 8;
    std::vector<stdx::thread> readers;
    readers.reserve(kReaders);

    for (int i = 0; i < kReaders; ++i) {
        readers.emplace_back([&] {
            // Mirror Span::_start: snapshot the provider and obtain a tracer from it. Refreshing
            // the snapshot and GetTracer's mutation of the provider's tracer list must be safe
            // against a concurrent shutdown().
            for (int iter = 0; iter < 10000; ++iter) {
                if (auto* provider = service->getTracerProvider()) {
                    auto tracer = provider->GetTracer("mongodb");
                    tracer->StartSpan(std::string{std::to_string(iter)}, {});
                }
            }
        });
    }

    // Tear the provider down while readers are still hitting it.
    service->shutdown();

    for (auto& reader : readers) {
        reader.join();
    }
}

/**
 * A SpanExporter whose Export() (invoked on the BatchSpanProcessor's background thread) reads a
 * heap object owned by the test. By destroying the `exportCanary`, tests can verify that no threads
 * call `Export` after a certain point.
 */
class CanaryExporter final : public opentelemetry::sdk::trace::SpanExporter {
public:
    explicit CanaryExporter(std::atomic<int>* exportCanary) : _canary(exportCanary) {}

    std::unique_ptr<opentelemetry::sdk::trace::Recordable> MakeRecordable() noexcept override {
        return std::make_unique<opentelemetry::sdk::trace::SpanData>();
    }

    opentelemetry::sdk::common::ExportResult Export(
        const opentelemetry::nostd::span<std::unique_ptr<opentelemetry::sdk::trace::Recordable>>&
            spans) noexcept override {
        // Reading through _canary after the test has freed it is a heap-use-after-free that ASAN
        // will flag. It is only reachable if the export thread is still alive post-shutdown.
        _canary->fetch_add(static_cast<int>(spans.size()));
        return opentelemetry::sdk::common::ExportResult::kSuccess;
    }

    bool ForceFlush(std::chrono::microseconds) noexcept override {
        return true;
    }

    bool Shutdown(std::chrono::microseconds) noexcept override {
        return true;
    }

private:
    std::atomic<int>* _canary;
};

TEST(TracerProviderServiceTest, ShutdownEndsExportThread) {
    // A heap-allocated canary the exporter dereferences on every Export(). Freeing it while the
    // export thread can still run turns "export thread outlived shutdown()" into a UAF bug ASAN
    // can catch.
    std::atomic<int> canary{0};

    // Build a provider whose processor never auto-flushes (huge delay), so the only export is the
    // one we drive explicitly below.
    opentelemetry::sdk::trace::BatchSpanProcessorOptions opts;
    opts.max_queue_size = 4096;
    opts.max_export_batch_size = 512;
    opts.schedule_delay_millis = std::chrono::hours{1};
    auto processor = opentelemetry::sdk::trace::BatchSpanProcessorFactory::Create(
        std::make_unique<CanaryExporter>(&canary), opts);
    auto provider = opentelemetry::sdk::trace::TracerProviderFactory::Create(std::move(processor));

    TracerProviderService service{nullptr};
    service.setTracerProvider_ForTest(std::move(provider));

    // Coordinate an "operation" thread (which reads the provider and later ends spans) with this
    // "shutdown" thread. Keeping the two on separate threads mirrors production and helps catch
    // potential threading bugs.
    unittest::Barrier providerPinned(2);
    unittest::Barrier shutdownComplete(2);

    unittest::threadAssertionMonitoredTest([&](unittest::ThreadAssertionMonitor& monitor) {
        auto opThread = monitor.spawn([&] {
            // Read the provider as if starting a span. These objects will not be freed until after
            // shutdown().
            auto* provider = service.getTracerProvider();
            ASSERT(provider);
            auto tracer = provider->GetTracer("mongodb");

            // These spans complete *after* shutdown(), on this thread. Some are started before
            // shutdown(), some after.
            std::vector<std::shared_ptr<opentelemetry::trace::Span>> spans;
            constexpr int kSpans = 256;
            spans.reserve(kSpans);
            for (int i = 0; i < kSpans / 2; ++i) {
                spans.push_back(tracer->StartSpan("egressCommand", {}));
            }

            providerPinned.countDownAndWait();
            shutdownComplete.countDownAndWait();

            for (int i = 0; i < kSpans / 2; ++i) {
                spans.push_back(tracer->StartSpan("egressCommand", {}));
            }
            for (auto& span : spans) {
                span->End();
            }

            // Drive the export. A correct shutdown() makes this a no-op (processor already shut
            // down).
            tracer->ForceFlush(std::chrono::seconds{5});
        });

        // Shutdown after the thread has pinned the provider and started some spans.
        providerPinned.countDownAndWait();
        service.shutdown();
        shutdownComplete.countDownAndWait();

        opThread.join();
    });
    EXPECT_EQ(canary.load(), 0);
}

}  // namespace
}  // namespace mongo::otel::traces
