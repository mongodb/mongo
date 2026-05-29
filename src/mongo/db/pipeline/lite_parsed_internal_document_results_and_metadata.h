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

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata_gen.h"
#include "mongo/db/pipeline/lite_parsed_document_source_nested_pipelines.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/pipeline/stage_params.h"

#include <memory>
#include <vector>

namespace mongo {

/**
 * Stage params produced by InternalDocumentResultsAndMetadataLiteParsed and consumed by
 * DocumentSourceInternalDocumentResultsAndMetadata::createFromStageParams.
 */
class InternalDocumentResultsAndMetadataStageParams : public StageParams {
public:
    InternalDocumentResultsAndMetadataStageParams(DocumentSourceResultsAndMetadataSpec spec)
        : _spec(std::move(spec)) {}

    static const Id& id;
    Id getId() const final {
        return id;
    }

    const DocumentSourceResultsAndMetadataSpec& getSpec() const {
        return _spec;
    }

private:
    DocumentSourceResultsAndMetadataSpec _spec;
};

/**
 * Lite-parsed representation of the $_internalDocumentResultsAndMetadata stage.
 */
class InternalDocumentResultsAndMetadataLiteParsed final
    : public LiteParsedDocumentSourceNestedPipelines<InternalDocumentResultsAndMetadataLiteParsed> {
public:
    static std::unique_ptr<InternalDocumentResultsAndMetadataLiteParsed> parse(
        const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options);

    InternalDocumentResultsAndMetadataLiteParsed(const BSONElement& spec,
                                                 DocumentSourceResultsAndMetadataSpec parsedSpec,
                                                 OwnedLiteParsedPipeline sourcePipeline)
        : LiteParsedDocumentSourceNestedPipelines(
              spec, boost::none, std::vector<OwnedLiteParsedPipeline>{std::move(sourcePipeline)}),
          _parsedSpec(std::move(parsedSpec)) {}

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final {
        return requiredPrivilegesBasic(isMongos, bypassDocumentValidation);
    }

    bool isInitialSource() const final {
        return true;
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        return std::make_unique<InternalDocumentResultsAndMetadataStageParams>(_parsedSpec);
    }

private:
    DocumentSourceResultsAndMetadataSpec _parsedSpec;
};

}  // namespace mongo
