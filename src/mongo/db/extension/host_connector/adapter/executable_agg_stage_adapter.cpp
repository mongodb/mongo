// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host_connector/adapter/executable_agg_stage_adapter.h"

#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo::extension::host_connector {

::MongoExtensionStatus* HostExecAggStageAdapter::_hostGetNext(
    ::MongoExtensionExecAggStage* execAggStage,
    ::MongoExtensionQueryExecutionContext* execCtxPtr,
    ::MongoExtensionGetNextResult* apiResult) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        apiResult->code = ::MongoExtensionGetNextResultCode::kPauseExecution;
        apiResult->resultDocument = createEmptyByteContainer();
        apiResult->resultMetadata = createEmptyByteContainer();

        auto& aggStageAdapter = *static_cast<HostExecAggStageAdapter*>(execAggStage);

        // Keep the last getNextResult stable.
        aggStageAdapter._lastGetNextResult =
            CachedGetNextResult(aggStageAdapter.getImpl().getNext());
        const auto hostStatusCode = aggStageAdapter._lastGetNextResult.getStatus();
        // Shouldn't need to validate that the hostResult has a valid Document when the status
        // is kAdvanced because the exec agg stage's implementation on the server should take
        // care of that validation.
        switch (hostStatusCode) {
            using ReturnStatus = exec::agg::GetNextResult::ReturnStatus;
            case ReturnStatus::kAdvanced: {
                apiResult->code = ::MongoExtensionGetNextResultCode::kAdvanced;
                aggStageAdapter._lastGetNextResult.getAsExtensionNextResult(*apiResult);
                break;
            }
            case ReturnStatus::kPauseExecution: {
                apiResult->code = ::MongoExtensionGetNextResultCode::kPauseExecution;
                break;
            }
            case ReturnStatus::kEOF: {
                apiResult->code = ::MongoExtensionGetNextResultCode::kEOF;
                break;
            }
            default: {
                tasserted(11019500,
                          str::stream()
                              << "Cannot forward a GetNextResult with the following ReturnStatus: "
                              << static_cast<int>(hostStatusCode));
                break;
            }
        }
    });
}

::MongoExtensionByteView HostExecAggStageAdapter::_hostGetName(
    const ::MongoExtensionExecAggStage* execAggStage) noexcept {
    auto sv = static_cast<const HostExecAggStageAdapter*>(execAggStage)->getImpl().getName();
    std::string_view sd{sv.data(), sv.length()};
    return stringDataAsByteView(sd);
}

void CachedGetNextResult::getAsExtensionNextResult(::MongoExtensionGetNextResult& outputResult) {
    // First, we must obtain the BSONObj for the document.
    // Whether the resultDocument BSON is owned or not owned, it does not matter, since we
    // will keep it stable for the lifetime of this cached result.
    if (!_resultDocument) {
        _resultDocument = _getNextResult.getDocument().toBson();
    }
    if (!_resultMetadata) {
        _resultMetadata = _getNextResult.getDocument().toBsonWithMetaDataOnly();
    }
    // Populate the output result as a view on the cached result BSON.
    outputResult.resultDocument.type = MongoExtensionByteContainerType::kByteView;
    outputResult.resultDocument.bytes.view = objAsByteView(*_resultDocument);
    outputResult.resultMetadata.type = MongoExtensionByteContainerType::kByteView;
    outputResult.resultMetadata.bytes.view = objAsByteView(*_resultMetadata);
}

};  // namespace mongo::extension::host_connector
