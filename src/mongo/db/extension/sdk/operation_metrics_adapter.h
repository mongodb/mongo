// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo::extension::sdk {

/**
 * OperationMetricsBase is the base class for implementing the
 * ::MongoExtensionOperationMetrics interface by an extension.
 *
 * An extension must provide a specialization of this base class, and
 * expose it to the host as a OperationMetricsAdapter.
 */
class OperationMetricsBase {
public:
    virtual ~OperationMetricsBase() = default;
    virtual BSONObj serialize() const = 0;
    virtual void update(MongoExtensionByteView) = 0;
};

/**
 * OperationMetricsAdapter is a boundary object representation of a
 * ::MongoExtensionOperationMetrics. It is meant to abstract away the C++ implementation
 * by the extension and provides the interface at the API boundary which will be called upon by the
 * host. The static VTABLE member points to static methods which ensure the correct conversion from
 * C++ context to the C API context.
 *
 * This abstraction is required to ensure we maintain the public
 * ::MongoExtensionOperationMetrics interface and layout as dictacted by the public API.
 * Any polymorphic behavior must be deferred to and implemented by the underlying
 * OperationMetricsBase.
 */
class OperationMetricsAdapter final : public ::MongoExtensionOperationMetrics {
public:
    OperationMetricsAdapter(std::unique_ptr<OperationMetricsBase> metrics)
        : ::MongoExtensionOperationMetrics{&VTABLE}, _metrics(std::move(metrics)) {
        sdk_tassert(11417103, "Provided OperationMetricsBase is null", _metrics != nullptr);
    }

    ~OperationMetricsAdapter() = default;

    OperationMetricsAdapter(const OperationMetricsAdapter&) = delete;
    OperationMetricsAdapter& operator=(const OperationMetricsAdapter&) = delete;
    OperationMetricsAdapter(OperationMetricsAdapter&&) = delete;
    OperationMetricsAdapter& operator=(OperationMetricsAdapter&&) = delete;

    const OperationMetricsBase& getImpl() const noexcept {
        return *_metrics;
    }

    OperationMetricsBase& getImpl() noexcept {
        return *_metrics;
    }

private:
    static void _extDestroy(::MongoExtensionOperationMetrics* metrics) noexcept {
        delete static_cast<OperationMetricsAdapter*>(metrics);
    }

    static MongoExtensionStatus* _extSerialize(const ::MongoExtensionOperationMetrics* metrics,
                                               ::MongoExtensionByteBuf** output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            *output = nullptr;

            const auto& impl = static_cast<const OperationMetricsAdapter*>(metrics)->getImpl();

            *output = new ByteBuf(impl.serialize());
        });
    }

    static ::MongoExtensionStatus* _extUpdate(::MongoExtensionOperationMetrics* metrics,
                                              ::MongoExtensionByteView arguments) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            auto& impl = static_cast<OperationMetricsAdapter*>(metrics)->getImpl();
            impl.update(arguments);
        });
    };

    static constexpr ::MongoExtensionOperationMetricsVTable VTABLE = {
        .destroy = &_extDestroy, .serialize = &_extSerialize, .update = &_extUpdate};
    std::unique_ptr<OperationMetricsBase> _metrics;
};
}  // namespace mongo::extension::sdk
