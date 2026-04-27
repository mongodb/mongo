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
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_rank_fusion_gen.h"
#include "mongo/db/pipeline/lite_parsed_document_source_nested_pipelines.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/stage_params.h"

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class ExpressionContext;
class Pipeline;

/**
 * Stage params produced by LiteParsedRankFusion and consumed by
 * DocumentSourceRankFusion::createFromStageParams.
 */
class RankFusionStageParams : public StageParams {
public:
    RankFusionStageParams(RankFusionSpec spec,
                          const std::vector<LiteParsedPipeline>& pipelines,
                          BSONObj originalBson)
        : _spec(std::move(spec)), _originalBson(std::move(originalBson)) {
        _pipelines.reserve(pipelines.size());
        for (const auto& pipeline : pipelines) {
            _pipelines.push_back(pipeline.clone());
        }
    }

    static const Id& id;
    Id getId() const final {
        return id;
    }

    const RankFusionSpec& getSpec() const {
        return _spec;
    }
    const std::vector<LiteParsedPipeline>& getPipelines() const {
        return _pipelines;
    }
    BSONElement getOriginalBson() const {
        return _originalBson.firstElement();
    }

    /**
     * Builds the input pipelines map from the already-validated LiteParsedPipelines stored on
     * this object. Used by DocumentSourceRankFusion::createFromStageParams to avoid re-parsing
     * the IDL.
     */
    std::map<std::string, std::unique_ptr<Pipeline>> buildInputPipelines(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx) const;

private:
    RankFusionSpec _spec;
    std::vector<LiteParsedPipeline> _pipelines;
    BSONObj _originalBson;
};

/**
 * Lite-parsed representation of the $rankFusion stage.
 */
class LiteParsedRankFusion final
    : public LiteParsedDocumentSourceNestedPipelines<LiteParsedRankFusion> {
public:
    static std::unique_ptr<LiteParsedRankFusion> parse(const NamespaceString& nss,
                                                       const BSONElement& spec,
                                                       const LiteParserOptions& options);

    LiteParsedRankFusion(const BSONElement& spec,
                         const NamespaceString& nss,
                         RankFusionSpec parsedSpec,
                         std::vector<LiteParsedPipeline> pipelines)
        : LiteParsedDocumentSourceNestedPipelines(spec, nss, std::move(pipelines)),
          _parsedSpec(std::move(parsedSpec)) {}

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final {
        return requiredPrivilegesBasic(isMongos, bypassDocumentValidation);
    }

    bool requiresAuthzChecks() const override {
        return false;
    }

    bool isSearchStage() const final {
        return !_pipelines.empty() && _pipelines[0].hasSearchStage();
    }

    bool isHybridSearchStage() const final {
        return true;
    }

    // $rankFusion desugars into a pipeline that includes $sort.
    bool isRankedStage() const final {
        return true;
    }

    // $rankFusion computes a reciprocal rank fusion score for each document.
    bool isScoredStage() const final {
        return true;
    }

    // $rankFusion does not modify documents, only combines and reorders them.
    bool isSelectionStage() const final {
        return true;
    }

    Constraints constraints() const override {
        return {.canRunOnTimeseries = false};
    }

    void validate() const override;

    bool hasExtensionVectorSearchStage() const override {
        return std::any_of(_pipelines.begin(), _pipelines.end(), [](const auto& pipeline) {
            return pipeline.hasExtensionVectorSearchStage();
        });
    }

    bool hasExtensionSearchStage() const override {
        return std::any_of(_pipelines.begin(), _pipelines.end(), [](const auto& pipeline) {
            return pipeline.hasExtensionSearchStage();
        });
    }

    std::unique_ptr<StageParams> getStageParams() const override {
        return std::make_unique<RankFusionStageParams>(
            _parsedSpec, _pipelines, getOriginalBson().wrap().getOwned());
    }

private:
    RankFusionSpec _parsedSpec;
};

}  // namespace mongo
