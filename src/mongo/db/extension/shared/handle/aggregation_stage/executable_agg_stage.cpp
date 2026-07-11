// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/extension/shared/handle/aggregation_stage/executable_agg_stage.h"

#include "mongo/db/extension/shared/explain_utils.h"
#include "mongo/db/extension/shared/extension_status.h"

#include <string_view>

namespace mongo::extension {

void ExecAggStageAPI::setSource(const ExecAggStageHandle& input) {
    invokeCAndConvertStatusToException([&]() { return _vtable().set_source(get(), input.get()); });
}

ExtensionGetNextResult ExecAggStageAPI::getNext(MongoExtensionQueryExecutionContext* execCtxPtr) {
    ::MongoExtensionGetNextResult result = createDefaultExtensionGetNext();
    invokeCAndConvertStatusToException(
        [&]() { return _vtable().get_next(get(), execCtxPtr, &result); });

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
