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

#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"

namespace mongo {

OwnedLiteParsedPipeline::OwnedLiteParsedPipeline(const NamespaceString& nss,
                                                 const std::vector<BSONObj>& pipelineStages,
                                                 const LiteParserOptions& options)
    : _ownedStages(_makeStagesOwned(pipelineStages)),
      // Subpipelines are never the top-level view pipeline; pass false for the hybrid-search flag.
      _pipeline(nss, _ownedStages, /*isRunningAgainstView_ForHybridSearch=*/false, options) {}

OwnedLiteParsedPipeline::OwnedLiteParsedPipeline(const OwnedLiteParsedPipeline& other)
    // Each stage in the copy receives a fresh, independently-owned BSONObj: the pipeline is
    // cloned (copying each stage's parsed state), then makeOwned() calls _originalBson.wrap()
    // on each stage to produce a new self-sufficient BSONObj — the copy-ctor equivalent of
    // the primary ctor's _makeStagesOwned() pass. _ownedStages is left empty because each
    // stage carries its own BSON directly after makeOwned().
    : _ownedStages(), _pipeline(other._pipeline) {
    _pipeline.makeOwned();
}

// static
std::vector<BSONObj> OwnedLiteParsedPipeline::_makeStagesOwned(
    const std::vector<BSONObj>& pipelineStages) {
    std::vector<BSONObj> owned;
    owned.reserve(pipelineStages.size());
    for (const auto& stage : pipelineStages) {
        owned.push_back(stage.getOwned());
    }
    return owned;
}

}  // namespace mongo
