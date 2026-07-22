// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/tracer_provider_service.h"

#include "mongo/otel/traces/tracer_provider_service_factory.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

#include <vector>

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

}  // namespace
}  // namespace mongo::otel::traces
