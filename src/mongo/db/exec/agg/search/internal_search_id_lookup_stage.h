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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/exec_shard_filter_policy.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

class InternalSearchIdLookUpStage final : public Stage {
public:
    using SearchIdLookupMetrics = DocumentSourceInternalSearchIdLookUp::SearchIdLookupMetrics;

    InternalSearchIdLookUpStage(StringData stageName,
                                const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                long long limit,
                                ExecShardFilterPolicy shardFilterPolicy,
                                const std::shared_ptr<SearchIdLookupMetrics>& searchIdLookupMetrics,
                                std::unique_ptr<mongo::Pipeline> viewPipeline);

    const SpecificStats* getSpecificStats() const override {
        return &_stats;
    }

    Document getExplainOutput(
        const SerializationOptions& opts = SerializationOptions{}) const override;

private:
    GetNextResult doGetNext() final;

    const std::string _stageName;
    const long long _limit;
    ExecShardFilterPolicy _shardFilterPolicy;

    std::shared_ptr<SearchIdLookupMetrics> _searchIdLookupMetrics;
    DocumentSourceIdLookupStats _stats;

    // If a search query is run on a view, we store the parsed view pipeline.
    std::unique_ptr<mongo::Pipeline> _viewPipeline;
};

}  // namespace mongo::exec::agg
