// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_join_plan_cache_stats.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/query/plan_cache/join_plan_cache.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <deque>
#include <string>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(joinPlanCacheStats,
                                     DocumentSourceJoinPlanCacheStats::LiteParsed::parse,
                                     AllowedWithApiStrict::kNeverInVersion1);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(joinPlanCacheStats,
                                                   DocumentSourceJoinPlanCacheStats,
                                                   JoinPlanCacheStatsStageParams);

// Implements '$joinPlanCacheStats' on top of a 'DocumentSourceQueue', so no dedicated exec stage is
// required.
boost::intrusive_ptr<DocumentSource> DocumentSourceJoinPlanCacheStats::createFromBson(
    BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    // The join plan cache is only populated when join optimization and the join plan cache are
    // enabled, so gate the stage on both server parameters. These cannot conditionally register a
    // DocumentSource, so we register unconditionally and bail out here.
    auto& qkc = QueryKnobConfiguration::get(pExpCtx->getOperationContext());
    uassert(ErrorCodes::QueryFeatureNotAllowed,
            str::stream() << kStageName
                          << " requires 'internalEnableJoinOptimization' and "
                             "'internalEnableJoinPlanCache' to be enabled",
            qkc.isJoinOrderingEnabled() && qkc.getEnableJoinPlanCache());

    uassert(ErrorCodes::FailedToParse,
            str::stream() << kStageName
                          << " value must be an object. Found: " << typeName(spec.type()),
            spec.type() == BSONType::object);
    uassert(ErrorCodes::FailedToParse,
            str::stream() << kStageName << " does not accept any options",
            spec.embeddedObject().isEmpty());

    const NamespaceString& nss = pExpCtx->getNamespaceString();
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << kStageName
                          << " must be run against the 'admin' database with {aggregate: 1}",
            nss.isAdminDB() && nss.isCollectionlessAggregateNS());

    // The read is deferred so that it runs on whichever node actually executes the stage: on a
    // router the stage parses normally, and 'kTargetedShards' in constraints() then dispatches it
    // to every shard, where each one dumps its own join plan cache. Note the knob gate above
    // therefore also applies on the router, so the parameters must be enabled there as well.
    DocumentSourceQueue::DeferredQueue deferredQueue{[pExpCtx]() {
        auto* opCtx = pExpCtx->getOperationContext();

        // The join plan cache is a node-global ServiceContext decoration, so no collection
        // acquisition is required.
        auto entries = JoinPlanCache::get(opCtx->getServiceContext()).serializeEntries();

        // Each shard has its own join plan cache, so when returning results to a router augment
        // every entry with the name of the shard it came from.
        std::string shardName;
        if (pExpCtx->getFromRouter()) {
            shardName = pExpCtx->getMongoProcessInterface()->getShardName(opCtx);
            uassert(13127802,
                    "Aggregation request specified 'fromRouter' but unable to retrieve shard name "
                    "for $joinPlanCacheStats pipeline stage.",
                    !shardName.empty());
        }

        std::deque<DocumentSource::GetNextResult> queue;
        for (auto&& entry : entries) {
            MutableDocument doc{Document{std::move(entry)}};
            if (!shardName.empty()) {
                doc.setField("shard", Value{shardName});
            }
            queue.emplace_back(doc.freeze());
        }
        return queue;
    }};

    // 'DocumentSourceQueue::serialize()' would force the deferred queue above, reading the cache on
    // whichever node serializes the pipeline (e.g. a router). Serializing as a constant
    // '{$joinPlanCacheStats: {}}' avoids that, keeps the query shape stable, and is reparse-safe so
    // each shard re-reads its own cache locally.
    return make_intrusive<DocumentSourceQueue>(std::move(deferredQueue),
                                               pExpCtx,
                                               /* stageNameOverride */ kStageName,
                                               /* serializeOverride */ Value{spec.wrap()},
                                               /* constraintsOverride */ constraints());
}

}  // namespace mongo
