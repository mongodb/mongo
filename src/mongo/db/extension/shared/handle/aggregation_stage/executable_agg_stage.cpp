// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/extension/shared/handle/aggregation_stage/executable_agg_stage.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/shared/explain_utils.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/util/fail_point.h"

#include <string_view>

namespace mongo::extension {

// Test-only fail point that corrupts the GetNextResult the host sees, simulating an extension
// that returns an OK status with a kAdvanced result whose byte view is empty or dangling.
MONGO_FAIL_POINT_DEFINE(failExtensionGetNextInvalidResult);

void ExecAggStageAPI::setSource(const ExecAggStageHandle& input) {
    invokeCAndConvertStatusToException([&]() { return _vtable().set_source(get(), input.get()); });
}

ExtensionGetNextResult ExecAggStageAPI::getNext(MongoExtensionQueryExecutionContext* execCtxPtr) {
    ::MongoExtensionGetNextResult result = createDefaultExtensionGetNext();
    invokeCAndConvertStatusToException(
        [&]() { return _vtable().get_next(get(), execCtxPtr, &result); });

    failExtensionGetNextInvalidResult.execute([&](const BSONObj& data) {
        result.code = ::MongoExtensionGetNextResultCode::kAdvanced;
        auto& container = (data.getStringField("field") == "resultMetadata")
            ? result.resultMetadata
            : result.resultDocument;
        // Destroy a transferred kByteBuf before overwriting the container, else the owning
        // pointer is dropped and the buffer leaks.
        if (container.type == MongoExtensionByteContainerType::kByteBuf && container.bytes.buf) {
            ExtensionByteBufHandle{container.bytes.buf};
        }
        container.type = MongoExtensionByteContainerType::kByteView;
        container.bytes.view = ::MongoExtensionByteView{
            reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(data.getIntField("data"))),
            static_cast<size_t>(data.getIntField("len"))};
    });

    return ExtensionGetNextResult::makeFromApiResult(result);
}

std::string_view ExecAggStageAPI::getName() const {
    return byteViewAsStringView(_vtable().get_name(get()));
}

OwnedOperationMetricsHandle ExecAggStageAPI::createMetrics() const {
    MongoExtensionOperationMetrics* metrics = nullptr;
    invokeCAndConvertStatusToException([&]() { return _vtable().create_metrics(get(), &metrics); });

    tassert(ErrorCodes::ExtensionSerializationError,
            "Result of `create_metrics` was nullptr",
            metrics != nullptr);

    // Take ownership of the created metrics and return the result.
    return OwnedOperationMetricsHandle(metrics);
}

void ExecAggStageAPI::open() {
    invokeCAndConvertStatusToException([&]() { return _vtable().open(get()); });
}

void ExecAggStageAPI::reopen() {
    invokeCAndConvertStatusToException([&]() { return _vtable().reopen(get()); });
}

void ExecAggStageAPI::close() {
    invokeCAndConvertStatusToException([&]() { return _vtable().close(get()); });
}

BSONObj ExecAggStageAPI::explain(MongoExtensionQueryExecutionContext& execCtx,
                                 mongo::ExplainOptions::Verbosity verbosity) const {
    ::MongoExtensionByteBuf* buf{nullptr};
    invokeCAndConvertStatusToException([&]() {
        return _vtable().explain(
            get(), &execCtx, convertHostVerbosityToExtVerbosity(verbosity), &buf);
    });

    tassert(ErrorCodes::ExtensionSerializationError,
            "buffer returned from explain must not be null",
            buf);

    // Take ownership of the returned buffer so that it gets cleaned up, then retrieve an owned
    // BSONObj to return to the host.
    ExtensionByteBufHandle ownedBuf{buf};
    return bsonObjFromByteView(ownedBuf->getByteView()).getOwned();
}

}  // namespace mongo::extension
