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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_internal_all_collection_stats.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;
using std::list;

REGISTER_DOCUMENT_SOURCE(shardedDataDistribution,
                         DocumentSourceShardedDataDistribution::LiteParsed::parse,
                         DocumentSourceShardedDataDistribution::createFromBson,
                         AllowedWithApiStrict::kAlways);

list<intrusive_ptr<DocumentSource>> DocumentSourceShardedDataDistribution::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(6789100,
            "The $shardedDataDistribution stage specification must be an empty object",
            elem.type() == BSONType::object && elem.Obj().isEmpty());

    uassert(6789101,
            "The $shardedDataDistribution stage can only be run on router",
            expCtx->getInRouter());

    uassert(6789102,
            "The $shardedDataDistribution stage must be run on the admin database",
            expCtx->getNamespaceString().isAdminDB() &&
                expCtx->getNamespaceString().isCollectionlessAggregateNS());

    static const BSONObj kAllCollStatsObj =
        fromjson("{$_internalAllCollectionStats: {stats: {storageStats: {}}}}");

    // TODO (SERVER-92596): Remove `"storageStats.numOrphanDocs": 1` once the bug is fixed.
    static const BSONObj kProjectObj = fromjson(R"({
         $project: {
             "ns": 1,
             "shard": 1,
             "storageStats.numOrphanDocs": 1,
             "count": {$ifNull: ["$storageStats.count", 0]}, 
             "avgObjSize": {$ifNull: ["$storageStats.avgObjSize", 0]}, 
             "numOrphanDocs": {$ifNull: ["$storageStats.numOrphanDocs", 0]}, 
             "timeseries": {$ifNull: ["$storageStats.timeseries", null]}
         }
     })");

    // Compute the `numOrphanedDocs` and `numOwnedDocuments` fields.
    // Note that, for timeseries collections, these fields will report the number of buckets
    // instead of the number of documents. We've decided to keep the field names as they are to
    // avoid the downstream impact of having to check different fields depending on the collection
    // time.
    static const BSONObj kGroupObj = fromjson(R"({
        $group: {
            _id: "$ns",
            shards: {
                $push: {
                    $cond: {
                        if: {
                            $eq: ["$timeseries", null]
                        },
                        then: {
                            $let: {
                                vars: {
                                    nOwnedDocs: {
                                        $subtract: [
                                            "$count",
                                            "$numOrphanDocs"
                                        ]
                                    }
                                },
                                in: {
                                    shardName: "$shard",
                                    numOrphanedDocs: "$numOrphanDocs",
                                    numOwnedDocuments: "$$nOwnedDocs",
                                    ownedSizeBytes: {
                                        $multiply: [
                                            "$avgObjSize",
                                            "$$nOwnedDocs"
                                        ]
                                    },
                                    orphanedSizeBytes: {
                                        $multiply: [
                                            "$avgObjSize",
                                            "$numOrphanDocs"
                                        ]
                                    }
                                }
                            }
                        }, 
                        else: {
                            $let: {
                                vars: {
                                    nOwnedDocs: {
                                        $subtract: [
                                            "$timeseries.bucketCount",
                                            "$numOrphanDocs"
                                        ]
                                    }
                                },
                                in: {
                                    shardName: "$shard",
                                    numOrphanedDocs: "$numOrphanDocs",
                                    numOwnedDocuments: "$$nOwnedDocs",
                                    ownedSizeBytes: {
                                        $multiply: [
                                            "$timeseries.avgBucketSize",
                                            "$$nOwnedDocs"
                                        ]
                                    },
                                    orphanedSizeBytes: {
                                        $multiply: [
                                            "$timeseries.avgBucketSize",
                                            "$numOrphanDocs"
                                        ]
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    })");
    static const BSONObj kConfigCollectionsLookupObj = fromjson(R"({
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
    static const BSONObj kMatchObj = fromjson(R"({
        $match: {
            $and: [{
                matchingShardedCollection: {$ne: []}
            },
            {
                'matchingShardedCollection.unsplittable': {$ne: true}
            }]
            }
    })");
    // Adding a new field matchingShards that contains the shards that have a chunk
    static const BSONObj kConfigChunksLookupObj = fromjson(R"({
        "$lookup": {
            "from": {
                "db": "config",
                "coll": "chunks"
            },
            "localField": "matchingShardedCollection.uuid",
            "foreignField": "uuid",
            "pipeline" : [
                { "$project": { "shard": 1 } },
                {
                    "$group": {
                        "_id": null,
                        "shards": {
                            "$addToSet": "$shard"
                        }
                    }
                }
            ],
            "as": "matchingShards"
        }    
    })");

    // We get rid of the db primary shards that do not contain a chunk and don't have any orphaned
    // docs
    static const BSONObj kFinalProjectObj = fromjson(R"({
        $project: {
            _id: 0,
            ns: "$_id",
            shards: {
                $filter: {
                    input: "$shards",
                    as: "shard",
                    cond: {
                        $or: [
                            {
                                $in: ["$$shard.shardName", {$first: "$matchingShards.shards"}]
                            },
                            {
                                $ne: ["$$shard.numOrphanedDocs", 0]
                            }
                        ]

                    }
                }
            }
        }
    })");
    return {
        DocumentSourceInternalAllCollectionStats::createFromBsonInternal(
            kAllCollStatsObj.firstElement(), expCtx),
        DocumentSourceProject::createFromBson(kProjectObj.firstElement(), expCtx),
        DocumentSourceGroup::createFromBson(kGroupObj.firstElement(), expCtx),
        DocumentSourceLookUp::createFromBson(kConfigCollectionsLookupObj.firstElement(), expCtx),
        DocumentSourceMatch::createFromBson(kMatchObj.firstElement(), expCtx),
        DocumentSourceLookUp::createFromBson(kConfigChunksLookupObj.firstElement(), expCtx),
        DocumentSourceProject::createFromBson(kFinalProjectObj.firstElement(), expCtx)};
}
}  // namespace mongo
