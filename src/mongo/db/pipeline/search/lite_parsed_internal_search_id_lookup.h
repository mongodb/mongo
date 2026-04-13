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

#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup_gen.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * StageParams for DocumentSourceInternalSearchIdLookUp. This class encapsulates the parameters
 * needed to construct a DocumentSourceInternalSearchIdLookUp stage.
 */
class InternalSearchIdLookupStageParams : public StageParams {
public:
    InternalSearchIdLookupStageParams() = default;
    InternalSearchIdLookupStageParams(DocumentSourceIdLookupSpec spec)
        : ownedSpec(std::move(spec)) {}

    static const Id& id;
    Id getId() const final {
        return id;
    }

    const DocumentSourceIdLookupSpec ownedSpec;
};

class LiteParsedInternalSearchIdLookUp final
    : public LiteParsedDocumentSourceDefault<LiteParsedInternalSearchIdLookUp> {
public:
    static constexpr StringData kStageName = "$_internalSearchIdLookup"_sd;

    static std::unique_ptr<LiteParsedInternalSearchIdLookUp> parse(const NamespaceString& nss,
                                                                   const BSONElement& spec,
                                                                   const LiteParserOptions& opts) {
        uassert(ErrorCodes::FailedToParse,
                "$_internalSearchIdLookup specification must be an object",
                spec.type() == BSONType::object);

        BSONObj specObj = spec.Obj().getOwned();

        // Parse using IDL.
        auto idlSpec = DocumentSourceIdLookupSpec::parse(specObj, IDLParserContext(kStageName));

        return std::make_unique<LiteParsedInternalSearchIdLookUp>(std::move(idlSpec));
    }

    bool isInitialSource() const override {
        return false;
    }

    // All search stages are unsupported on timeseries collections.
    Constraints constraints() const override {
        return {.canRunOnTimeseries = false};
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        return std::make_unique<InternalSearchIdLookupStageParams>(_ownedSpec);
    }

    const DocumentSourceIdLookupSpec& getSpec() const {
        return _ownedSpec;
    }

    FirstStageViewApplicationPolicy getFirstStageViewApplicationPolicy() const override {
        return FirstStageViewApplicationPolicy::kDoNothing;
    }

    void bindViewInfo(const ViewInfo& viewInfo, const ResolvedNamespaceMap&) override {
        _ownedSpec.setViewPipeline(viewInfo.getSerializedViewPipeline());
    }

    LiteParsedInternalSearchIdLookUp(DocumentSourceIdLookupSpec spec)
        : LiteParsedDocumentSourceDefault(BSON(kStageName << spec.toBSON()).getOwned()),
          _ownedSpec(std::move(spec)) {}

private:
    DocumentSourceIdLookupSpec _ownedSpec;
};

}  // namespace mongo
