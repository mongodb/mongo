/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/extension/host/extension_host_utils.h"

#include "mongo/base/init.h"
#include "mongo/db/commands/server_status/server_status_metric.h"

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
