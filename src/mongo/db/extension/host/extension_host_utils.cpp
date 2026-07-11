// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host/extension_host_utils.h"

#include "mongo/base/init.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/extension/shared/extension_status.h"

namespace mongo::extension::host {

namespace {

class HostObservabilityContext : public mongo::extension::ObservabilityContext {
public:
    ~HostObservabilityContext() override {}

    void extensionSuccess() const noexcept override {
        _sExtensionSuccessCounter.increment();
    }

    void extensionError() const noexcept override {
        _sExtensionFailureCounter.increment();
    }

    void hostSuccess() const noexcept override {
        _sHostSuccessCounter.increment();
    }

    void hostError() const noexcept override {
        _sHostFailureCounter.increment();
    }

private:
    static inline Counter64& _sExtensionSuccessCounter =
        *MetricBuilder<Counter64>("extension.extensionSuccesses");
    static inline Counter64& _sExtensionFailureCounter =
        *MetricBuilder<Counter64>("extension.extensionFailures");

    static inline Counter64& _sHostSuccessCounter =
        *MetricBuilder<Counter64>("extension.hostSuccesses");
    static inline Counter64& _sHostFailureCounter =
        *MetricBuilder<Counter64>("extension.hostFailures");
};

MONGO_INITIALIZER(InitializeGlobalObservabilityContext)(InitializerContext*) {
    mongo::extension::setGlobalObservabilityContext(std::make_unique<HostObservabilityContext>());
};
}  // namespace

}  // namespace mongo::extension::host
