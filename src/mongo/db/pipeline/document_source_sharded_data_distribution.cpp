/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_sharded_data_distribution.h"

#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_internal_all_collection_stats.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/s/catalog/type_collection.h"

#include "mongo/logv2/log.h"

namespace mongo {

using boost::intrusive_ptr;
using std::list;

REGISTER_DOCUMENT_SOURCE(shardedDataDistribution,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceShardedDataDistribution::createFromBson,
                         AllowedWithApiStrict::kAlways);

list<intrusive_ptr<DocumentSource>> DocumentSourceShardedDataDistribution::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(6789100,
            "The $shardedDataDistribution stage specification must be an empty object",
            elem.type() == Object && elem.Obj().isEmpty());

    uassert(
        6789101, "The $shardedDataDistribution stage can only be run on mongoS", expCtx->inMongos);

    uassert(6789102,
            "The $shardedDataDistribution stage must be run on the admin database",
            expCtx->ns.isAdminDB() && expCtx->ns.isCollectionlessAggregateNS());

    // Add 'config.collections' as a resolved namespace
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces[NamespaceString::kConfigsvrCollectionsNamespace.coll()] = {
        NamespaceString::kConfigsvrCollectionsNamespace, std::vector<BSONObj>{}};
    expCtx->setResolvedNamespaces(resolvedNamespaces);

    static const BSONObj kAllCollStatsObj =
        fromjson("{$_internalAllCollectionStats: {stats: {storageStats: {}}}}}");
    static const BSONObj kGroupObj = fromjson(R"({
        $group: {
            _id: "$ns",
            shards: {
                $push: {
                    $let: {
                        vars: {
                            nOwnedDocs: {
                                $subtract: [
                                    "$storageStats.count",
                                    "$storageStats.numOrphanDocs"
                                ]
                            }
                        },
                        in: {
                            shardName: "$shard",
                            numOrphanedDocs: "$storageStats.numOrphanDocs",
                            numOwnedDocuments: "$$nOwnedDocs",
                            ownedSizeBytes: {
                                $multiply: [
                                    "$storageStats.avgObjSize",
                                    "$$nOwnedDocs"
                                ]
                            },
                            orphanedSizeBytes: {
                                $multiply: [
                                    "$storageStats.avgObjSize",
                                    "$storageStats.numOrphanDocs"
                                ]
                            }
                        }
                    }
                }
            }
        }
    })");
    static const BSONObj kLookupObj = fromjson(R"({
         $lookup: {
            from: {
                db: "config",
                coll: "collections"
            },
            localField: "_id",
            foreignField: "_id",
            as: "matchingShardedCollection"
        }
    })");
    static const BSONObj kMatchObj = fromjson("{$match: {matchingShardedCollection: {$ne: []}}}");
    static const BSONObj kProjectObj = fromjson(R"({
        $project: {
            _id: 0,
            ns: "$_id",
            shards: "$shards"
        }
    })");

    return {DocumentSourceInternalAllCollectionStats::createFromBsonInternal(
                kAllCollStatsObj.firstElement(), expCtx),
            DocumentSourceGroup::createFromBson(kGroupObj.firstElement(), expCtx),
            DocumentSourceLookUp::createFromBson(kLookupObj.firstElement(), expCtx),
            DocumentSourceMatch::createFromBson(kMatchObj.firstElement(), expCtx),
            DocumentSourceProject::createFromBson(kProjectObj.firstElement(), expCtx)};
}
}  // namespace mongo
