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
#include "mongo/db/extension/public/extension_error_types_gen.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"

namespace mongo::extension::host_connector {

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
                  "Extension encountered error: " + exceptionInfo.getMessage());
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

        uassert(11134200,
                "create_id_lookup requires a well-formed $_internalSearchIdLookup",
                specObj.nFields() == 1 &&
                    specObj.firstElementFieldNameStringData() ==
                        DocumentSourceInternalSearchIdLookUp::kStageName &&
                    specObj.firstElementType() == BSONType::object);

        // Extract the inner spec object from the full stage BSON.
        auto innerSpec = specObj.firstElement().Obj().getOwned();
        auto spec = DocumentSourceIdLookupSpec::parseOwned(
            std::move(innerSpec),
            IDLParserContext(DocumentSourceInternalSearchIdLookUp::kStageName));

        // Use the full stage BSON element for the LiteParsed constructor.
        auto liteParsed = std::make_unique<LiteParsedInternalSearchIdLookUp>(std::move(spec));
        liteParsed->makeOwned();

        *node = static_cast<::MongoExtensionAggStageAstNode*>(new host::HostAggStageAstNodeAdapter(
            std::make_unique<host::AggStageAstNode>(std::move(liteParsed))));
    });
}
}  // namespace mongo::extension::host_connector
