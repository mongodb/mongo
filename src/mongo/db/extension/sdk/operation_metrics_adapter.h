/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#pragma once
#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/public/api.h"
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
        : ::MongoExtensionOperationMetrics{&VTABLE}, _metrics(std::move(metrics)) {}
    ~OperationMetricsAdapter() = default;

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

            *output = new VecByteBuf(impl.serialize());
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
