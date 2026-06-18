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
#include "mongo/db/extension/host_connector/adapter/host_services_adapter.h"

#include "mongo/db/extension/host/aggregation_stage/ast_node.h"
#include "mongo/db/extension/host/aggregation_stage/parse_node.h"
#include "mongo/db/extension/host/query_execution_context.h"
#include "mongo/db/extension/host_connector/adapter/query_execution_context_adapter.h"
#include "mongo/db/extension/public/extension_error_types_gen.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/lite_parsed_internal_search_id_lookup.h"

#include <string_view>

namespace mongo::extension::host_connector {

namespace {
/**
 * Throws with 'code' and 'message' unless 'specObj' is a well-formed single-stage spec of the form
 * {<stageName>: {...}}.
 */
void uassertWellFormedStageSpec(const BSONObj& specObj,
                                std::string_view stageName,
                                int code,
                                std::string_view message) {
    uassert(code,
            message,
            specObj.nFields() == 1 && specObj.firstElementFieldNameStringData() == stageName &&
                specObj.firstElementType() == BSONType::object);
}
}  // namespace

::MongoExtensionStatus* HostServicesAdapter::_extUserAsserted(
    ::MongoExtensionByteView structuredErrorMessage) {
    // We throw the exception here so that we get a stack trace that looks like a host exception but
    // originates from within the extension, so that we have a complete stack trace for diagnostic
    // information. At the same time, we are not allowed to throw an exception across the API
    // boundary, so we immediately convert this to a MongoExtensionStatus. It will be rethrown after
    // being passed through the boundary.
    return wrapCXXAndConvertExceptionToStatus([&]() {
        BSONObj errorBson = bsonObjFromByteView(structuredErrorMessage);
        auto exceptionInfo = mongo::extension::ExtensionExceptionInformation::parse(
            errorBson, IDLParserContext("extUassert"));

        // Call the host's uassert implementation.
        uasserted(exceptionInfo.getErrorCode(), exceptionInfo.getMessage());
    });
}

::MongoExtensionStatus* HostServicesAdapter::_extTripwireAsserted(
    ::MongoExtensionByteView structuredErrorMessage) {
    // We follow the same throw-then-catch pattern here as in _extUserAsserted, for the same
    // reasons.
    return wrapCXXAndConvertExceptionToStatus([&]() {
        BSONObj errorBson = bsonObjFromByteView(structuredErrorMessage);
        auto exceptionInfo = mongo::extension::ExtensionExceptionInformation::parse(
            errorBson, IDLParserContext("extTassert"));

        // Call the host's tassert implementation.
        tasserted(exceptionInfo.getErrorCode(),
                  "Extension encountered error: " + std::string{exceptionInfo.getMessage()});
    });
}

::MongoExtensionStatus* HostServicesAdapter::_extCreateHostAggStageParseNode(
    ::MongoExtensionByteView spec, ::MongoExtensionAggStageParseNode** node) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        *node = nullptr;
        auto parseNode = std::make_unique<host::AggStageParseNode>(bsonObjFromByteView(spec));
        *node = static_cast<::MongoExtensionAggStageParseNode*>(
            new host::HostAggStageParseNodeAdapter(std::move(parseNode)));
    });
}

::MongoExtensionStatus* HostServicesAdapter::_extCreateIdLookup(
    ::MongoExtensionByteView bsonSpec, ::MongoExtensionAggStageAstNode** node) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        *node = nullptr;
        BSONObj specObj = bsonObjFromByteView(bsonSpec).getOwned();

        uassertWellFormedStageSpec(
            specObj,
            DocumentSourceInternalSearchIdLookUp::kStageName,
            11134200,
            "create_id_lookup requires a well-formed $_internalSearchIdLookup");

        // Extract the inner spec object from the full stage BSON.
        auto innerSpec = specObj.firstElement().Obj().getOwned();
        auto spec = DocumentSourceIdLookupSpec::parseOwned(
            std::move(innerSpec),
            IDLParserContext(DocumentSourceInternalSearchIdLookUp::kStageName));

        // Use the full stage BSON element for the LiteParsed constructor.
        auto liteParsed = std::make_unique<LiteParsedInternalSearchIdLookUp>(std::move(spec));
        liteParsed->makeOwned();

        *node = static_cast<::MongoExtensionAggStageAstNode*>(new host::HostAggStageAstNodeAdapter(
            std::make_unique<host::IdLookupAstNode>(std::move(liteParsed))));
    });
}

::MongoExtensionStatus* HostServicesAdapter::_extCreateDocumentResultsAndMetadata(
    ::MongoExtensionByteView bsonSpec,
    ::MongoExtensionDocResultsDPLCallback dplCallback,
    void* dplCallbackUserData,
    void (*dplCallbackDestroy)(void*),
    ::MongoExtensionAggStageAstNode** node) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        *node = nullptr;

        // Wrap the raw C callback and destroy hook into std::function up front, before anything
        // that can throw, so the deleter runs exactly once even when stage-spec parsing or
        // validation below fails (otherwise a malformed bsonSpec would leak userData).
        host::DPLCallbackOwner::CallbackInvoker invoker;
        if (dplCallback) {
            invoker = [dplCallback, dplCallbackUserData](
                          ExpressionContext* expCtx,
                          ::MongoExtensionByteBuf** rawSort,
                          ::MongoExtensionByteBuf** rawMerge) -> ::MongoExtensionStatus* {
                auto wrappedCtx = std::make_unique<host::QueryExecutionContext>(expCtx);
                QueryExecutionContextAdapter execCtxAdapter(std::move(wrappedCtx));
                return dplCallback(dplCallbackUserData, &execCtxAdapter, rawSort, rawMerge);
            };
        }
        std::function<void()> deleter;
        if (dplCallbackDestroy) {
            deleter = [dplCallbackDestroy, dplCallbackUserData]() {
                dplCallbackDestroy(dplCallbackUserData);
            };
        }
        host::DPLCallbackOwner dplOwner(std::move(invoker), std::move(deleter));

        // The DocumentResultsAndMetadataAstNode constructor takes its own owned copy, so the view
        // here does not need to be owned.
        BSONObj specObj = bsonObjFromByteView(bsonSpec);

        uassertWellFormedStageSpec(specObj,
                                   DocumentSourceInternalDocumentResultsAndMetadata::kStageName,
                                   12601501,
                                   "create_document_results_and_metadata requires a well-formed "
                                   "$_internalDocumentResultsAndMetadata stage");

        *node = static_cast<::MongoExtensionAggStageAstNode*>(new host::HostAggStageAstNodeAdapter(
            std::make_unique<host::DocumentResultsAndMetadataAstNode>(std::move(specObj),
                                                                      std::move(dplOwner))));
    });
}
}  // namespace mongo::extension::host_connector
