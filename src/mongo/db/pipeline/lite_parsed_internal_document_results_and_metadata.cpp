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

#include "mongo/db/pipeline/lite_parsed_internal_document_results_and_metadata.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/util/str.h"

namespace mongo {

ALLOCATE_STAGE_PARAMS_ID(_internalDocumentResultsAndMetadata,
                         InternalDocumentResultsAndMetadataStageParams::id);

std::unique_ptr<InternalDocumentResultsAndMetadataLiteParsed>
InternalDocumentResultsAndMetadataLiteParsed::parse(const NamespaceString& nss,
                                                    const BSONElement& spec,
                                                    const LiteParserOptions& options) {
    tassert(ErrorCodes::FailedToParse,
            str::stream() << "$_internalDocumentResultsAndMetadata specification must be an "
                             "object, found "
                          << typeName(spec.type()),
            spec.type() == BSONType::object);

    auto parsedSpec = DocumentSourceResultsAndMetadataSpec::parse(
        spec.embeddedObject(), IDLParserContext("$_internalDocumentResultsAndMetadata"));

    auto sourceElem = spec.embeddedObject()["source"];
    tassert(ErrorCodes::FailedToParse,
            "$_internalDocumentResultsAndMetadata requires a 'source' field",
            !sourceElem.eoo());
    tassert(ErrorCodes::FailedToParse,
            "$_internalDocumentResultsAndMetadata 'source' must be an object",
            sourceElem.type() == BSONType::object);

    return std::make_unique<InternalDocumentResultsAndMetadataLiteParsed>(
        spec,
        std::move(parsedSpec),
        OwnedLiteParsedPipeline(nss, {sourceElem.embeddedObject()}, options));
}

}  // namespace mongo
