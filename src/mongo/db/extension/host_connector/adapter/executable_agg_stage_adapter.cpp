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

#include "mongo/db/extension/host_connector/adapter/executable_agg_stage_adapter.h"

#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/util/assert_util.h"

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
    StringData sd{sv.data(), sv.length()};
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
