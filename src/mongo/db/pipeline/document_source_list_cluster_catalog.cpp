/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_list_cluster_catalog.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_internal_list_collections.h"
#include "mongo/db/pipeline/document_source_list_cluster_catalog_gen.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/util/assert_util.h"

#include <cstddef>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;
using std::list;

class ListClusterCatalogPipelineBuilder {
public:
    ListClusterCatalogPipelineBuilder(const intrusive_ptr<ExpressionContext>& expCtx)
        : _expCtx(expCtx) {}

    template <class T>
    void addStage(const StringData el) {
        _pipeline.push_back(T::createFromBson(fromjson(el).firstElement(), _expCtx));
    }

    list<intrusive_ptr<DocumentSource>> finish() {
        return std::move(_pipeline);
    }

private:
    const intrusive_ptr<ExpressionContext>& _expCtx;
    list<intrusive_ptr<DocumentSource>> _pipeline;
};

std::string getDefaultMaxChunkSize(OperationContext* opCtx) {
    auto grid = Grid::get(opCtx);
    if (grid && grid->isInitialized()) {
        auto balancerConfig = grid->getBalancerConfiguration();
        balancerConfig->refreshAndCheck(opCtx).ignore();
        auto resultInMb =
            balancerConfig->getMaxChunkSizeBytes() / (static_cast<uint64_t>(1024 * 1024));
        return std::to_string(resultInMb);
    }
    return std::string("null");
}

REGISTER_DOCUMENT_SOURCE(listClusterCatalog,
                         DocumentSourceListClusterCatalog::LiteParsed::parse,
                         DocumentSourceListClusterCatalog::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);

list<intrusive_ptr<DocumentSource>> DocumentSourceListClusterCatalog::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {

    uassert(9621301,
            "The $listClusterCatalog stage must run on a database, not a collection.",
            expCtx->getNamespaceString().isCollectionlessAggregateNS());

    uassert(9621302,
            str::stream() << kStageName << " must take an object but found: " << elem,
            elem.type() == BSONType::object);
    /**
     * Compose the pipeline to generate an entry for each existing collection and its related
     * sharding informations.
     * The stage will:
     *  - Report any collection in the catalog if the stage runs against the `admin` db
     *  - Report all the collections for a specific db if the stage runs against a specific db.
     *
     * Pipeline stages and their purposes:
     * 1. $_internalListCollection: One entry per collection according to the database. The primary
     * db for every collection is also reported.
     *
     * 2. $lookup Stage (config.collections):
     *    - Joins collection information with config.collections
     *    - Retrieves tracking and sharding configuration for each collection
     *    - Adds 'sharded' field: true if collection is sharded
     *    - Adds 'tracked' field: true if collection is tracked (unsharded or sharded)
     *    - Adds 'shardKey' field for sharded collections
     *
     * 3. Optional $lookup Stage (config.chunks) if `shards` spec is specified:
     *    - Groups chunks by shard to get shard list
     *    - For tracked collections: return the shard list
     *    - For non-tracked: return the primary shard only as a 1 item list
     *
     * 4. Optional $addField if `balancingConfiguration` spec is specified:
     *    - Adds 'balancingEnabled': true if collection is sharded and not explicitly disabled
     *    - Adds 'autoMergingEnabled': true if collection is sharded and auto-merge not disabled
     *    - Adds 'chunkSize': collection-specific or cluster-default chunk size in MB
     *
     * 5. Optional $project if `tracked` is not specified:
     *    - Remove the `tracked` field from any entry
     * 6. Cleanup:
     *    - Removes internal working fields (trackedCollectionInfo, firstTrackedCollectionInfo,
     * etc.)
     *
     */

    ListClusterCatalogPipelineBuilder pipeline(expCtx);
    pipeline.addStage<DocumentSourceInternalListCollections>(R"({$_internalListCollection:{}})");

    pipeline.addStage<DocumentSourceLookUp>(R"({
        $lookup : {
            from : {db : "config", coll : "collections"},
            localField : "ns",
            foreignField : "_id",
            as : "trackedCollectionInfo"
        } })");

    pipeline.addStage<DocumentSourceAddFields>(R"({
        $addFields: {
            firstTrackedCollectionInfo: {
                $first: "$trackedCollectionInfo"
            }
        }
    })");

    pipeline.addStage<DocumentSourceAddFields>(R"({ 
        $addFields: {
                "sharded": {
                        $and: [
                        { $ne: ["$trackedCollectionInfo", []] },
                        { $ne: [ "$firstTrackedCollectionInfo.unsplittable", true] }
                        ]
                },
                "tracked": { $ne: ["$trackedCollectionInfo", []] }
        }})");

    pipeline.addStage<DocumentSourceAddFields>(R"({ 
        $addFields: {
                "shardKey": {
                        $cond: {
                                if: "$sharded",
                                then: "$firstTrackedCollectionInfo.key",
                                else: "$$REMOVE"
                        }
                }
        } })");


    auto specs = mongo::DocumentSourceListClusterCatalogSpec::parse(elem.embeddedObject(),
                                                                    IDLParserContext(kStageName));
    if (specs.getShards()) {
        // Add a lookup stage to join with config.chunks
        pipeline.addStage<DocumentSourceLookUp>(R"({
            $lookup: {
                from: {
                    db: "config",
                    coll: "chunks"
                },
                localField: "info.uuid",
                foreignField: "uuid",
                pipeline : [
                    {
                        $group: {
                            _id: null,
                            "shards": {
                                "$addToSet": "$shard"
                            }
                        }
                    }
                ],
                as: "matchingShards"
            }
        })");

        pipeline.addStage<DocumentSourceAddFields>(R"({
            $addFields: {  
                 "shards": {  
                      $cond: {  
                           if: "$tracked",  
                           then: {$first: "$matchingShards.shards"},  
                           else: {$cond: ["$primary", ["$primary"], []]}
                       }
                   }
               }  
           })");
    }

    // TODO (SERVER-61033) balancingEnabled should stop depening on `permitMigrations`.
    // TODO (SERVER-61033) Remove balancingEnabledReason field.
    if (specs.getBalancingConfiguration()) {
        const auto maxChunkSizeInMb = getDefaultMaxChunkSize(expCtx->getOperationContext());
        pipeline.addStage<DocumentSourceAddFields>(R"({
        $addFields: {
                "balancingEnabled": {
                    $cond: {
                        if: "$sharded",
                        then: {
                            $and:[
                                {$not: {$ifNull: ["$firstTrackedCollectionInfo.noBalance", false]}},
                                {$ifNull: ["$firstTrackedCollectionInfo.permitMigrations", true]}
                            ]
                        },
                        else: "$$REMOVE"
                    }
                },
                "balancingEnabledReason": {
                    $cond: {
                        if: "$sharded",
                        then: {
                            "enableBalancing": {$not: {$ifNull: ["$firstTrackedCollectionInfo.noBalance", false]}},
                            "allowMigrations": {$ifNull: ["$firstTrackedCollectionInfo.permitMigrations", true]}
                        },
                        else: "$$REMOVE"
                    }
                },
                "autoMergingEnabled": {
                    $cond: {
                        if: "$sharded",
                        then: {
                            $ne: [ "$firstTrackedCollectionInfo.enableAutoMerge", false]
                        },
                        else: "$$REMOVE"
                    }
                },
                "chunkSize": {
                    $cond: {
                        if: "$sharded",
                        then: {
                            "$ifNull": [
                                {
                                    "$divide": [ "$firstTrackedCollectionInfo.maxChunkSizeBytes", 1048576]
                                },)" + maxChunkSizeInMb +
                                                   R"(]
                        },
                        else: "$$REMOVE"
                    }
                }
            }
        })");
    }
    if (!specs.getTracked()) {
        pipeline.addStage<DocumentSourceProject>(R"({
                $project: {
                    tracked: 0
                }
            })");
    }
    pipeline.addStage<DocumentSourceProject>(R"({
                $project: {
                    trackedCollectionInfo: 0,
                    firstTrackedCollectionInfo: 0,
                    matchingShards: 0,
                    primary: 0
                }
            })");


    return pipeline.finish();
}
}  // namespace mongo
