import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

export var RoutingTableConsistencyChecker = (function() {
    const sameObjectFields = (lhsObjFields, rhsObjFields) => {
        if (lhsObjFields.length !== rhsObjFields.length) {
            return false;
        }

        for (let i = 0; i != lhsObjFields.length; ++i) {
            if (lhsObjFields[i] !== rhsObjFields[i]) {
                return false;
            }
        }

        return true;
    };

    const fetchRoutingTableData = (mongos) => {
        // Group docs in config.chunks by coll UUID (sorting by minKey), then join with docs in
        // config.collections.
        return mongos.getDB('config')
                .chunks
                .aggregate([
                    { $sort: { min: 1 } },
                    {
                        $group: {
                            _id: '$uuid',
                            routingTable: { $push: '$$ROOT' },
                        }
                    },
                    {
                        $lookup: {
                            from: 'collections',
                            localField: '_id',
                            foreignField: 'uuid',
                            as: 'details'
                        },
                    }
                ], {readConcern: {level: "snapshot"}});
    };

    const checkCollRoutingTable = (nss, shardKeyPattern, routingTable) => {
        if (!routingTable) {
            jsTest.log(`config.collections entry '${nss}' has no chunks`);
            return false;
        }

        const shardKeyfields = Object.keys(shardKeyPattern);
        let lastLowerBound = {};
        for (const key of shardKeyfields) {
            lastLowerBound[key] = MinKey;
        }
        let expectedLastBound = {};
        for (const key of shardKeyfields) {
            expectedLastBound[key] = MaxKey;
        }
        for (const chunk of routingTable) {
            if (!sameObjectFields(Object.keys(chunk.min), shardKeyfields) ||
                !sameObjectFields(Object.keys(chunk.max), shardKeyfields)) {
                jsTest.log(`Shard key pattern violation found in config.chunks for ${
                    nss}! Expected: ${tojson(shardKeyPattern)}, found chunk ${tojson(chunk)}`);
                return false;
            }

            if (bsonWoCompare(chunk.min, lastLowerBound) !== 0) {
                jsTest.log(`Found gap or range overlap in config.chunks for collection ${
                    nss}, chunk ${tojson(chunk._id)}! Expected ${tojson(lastLowerBound)}, found ${
                    tojson(chunk.min)}`);
                return false;
            }
            lastLowerBound = chunk.max;
        }

        if (bsonWoCompare(routingTable[routingTable.length - 1].max, expectedLastBound) !== 0) {
            jsTest.log(
                `Incomplete range key detected in config.chunks for ${nss} (MaxKey missing)`);
            return false;
        }

        jsTest.log(`${nss} with ${routingTable.length} chunks passed the check`);
        return true;
    };

    /**
     * For each existing namespace at the time of the invocation, retrieves metadata (uuid and
     * placement at shard level) from both its routing table and the information persisted
     * config.placementHistory, represented as in the following schema:
     * {
     *   nss : <string>
     *   placement: <array of {uuid, shards, source [routing table | placement history] } objects>
     * }
     **/
    const collectPlacementMetadataByNamespace = (mongos) => {
        let pipeline = [
            // 1.1  Current placement metadata on existing collections
            {
                $lookup: {
                    from: "chunks",
                    localField: "uuid",
                    foreignField: "uuid",
                    as: "shardsOwningCollectionChunks",
                    pipeline: [
                        {
                            $group: {
                                _id: "$shard"
                            }
                        }
                    ],
                }
            },
            {
                $project: {
                    _id: 0,
                    nss: "$_id",
                    placement: {
                        source: "routing table",
                        shards: "$shardsOwningCollectionChunks._id",
                        uuid: "$uuid",
                    }
                }
            },
            // 1.2 Current placement metadata on existing databases
            {
                $unionWith: {
                    coll: "databases",
                    pipeline: [
                        {
                            $project: {
                                _id: 0,
                                nss: "$_id",
                                placement: {
                                    source: "routing table",
                                    shards: [
                                        "$primary"
                                    ]
                                }
                            }
                        }
                    ]
                }
            },
            // 2. Outer join with config.placementHistory to retrieve the most recent document 
            //    for each logged namespace.
            {
                $unionWith: {
                    coll: "placementHistory",
                    pipeline: [
                        {
                            $match: {
                                // 2.1 The initialization markers of config.placementHistory are skipped.
                                nss: {
                                    $ne: ""
                                }
                            }
                        },
                        {
                            $group: {
                                _id: "$nss",
                                placement: {
                                    $top: {
                                        output: "$$CURRENT",
                                        sortBy: {
                                            "timestamp": -1
                                        }
                                    }
                                }
                            }
                        },
                        // 2.2 Namespaces that are recorded as "dropped" are also skipped.      
                        { $match: { "placement.shards": { $not: { $size: 0 } } } },
                        {
                            $project: {
                                _id: 0,
                                nss: "$_id",
                                "placement.uuid": 1,
                                "placement.shards": 1,
                                "placement.source": "placement history"
                            }
                        }
                    ]
                }
            },
            // 3. Merge current and historical placement metadata on a namespace into a single doc.
            {
                $group: {
                    _id: "$nss",
                    placement: {
                        $push: "$placement"
                    }
                }
            }
        ];

        // Sharding metadata on temporary resharding collections require special treatment:
        // - when created by the server code, they only include routing information (but no
        // historical placement metadata)
        // - when forged by the test code (usually through shardCollection()) they behave as a
        // regular collection. The pipeline needs hence to be modified as follows:
        const tempReshardingCollectionsFilter = {
            $match: {_id: {$not: {$regex: /^[^.]+\.system\.resharding\..+$/}}}
        };
        if (TestData.mayForgeReshardingTempCollections) {
            // Perform no check on the temporary collections by adding a filter to
            // the fully-outer-joined data.
            pipeline.push(tempReshardingCollectionsFilter);
        } else {
            // Only filter out temporary collections from config.collections; this will allow to
            // check that such namespaces are not mentioned in config.placementHistory.
            pipeline.unshift(tempReshardingCollectionsFilter);
        }

        return mongos.getDB('config').collections.aggregate(pipeline,
                                                            {readConcern: {level: "snapshot"}});
    };

    const checkHistoricalPlacementMetadataConsistency = (mongos) => {
        const metadataByNamespace = collectPlacementMetadataByNamespace(mongos);

        metadataByNamespace.forEach(function(namespaceMetadata) {
            // Invariant check.
            assert(namespaceMetadata.placement.length === 1 ||
                       namespaceMetadata.placement.length === 2,
                   `Unexpected output format from collectPlacementMetadataByNamespace(): ${
                       tojson(namespaceMetadata)}`);
            // Information missing from either the routing table or placement history.
            assert(namespaceMetadata.placement.length === 2,
                   `Incomplete placement metadata for namespace ${
                       namespaceMetadata._id}. Details: ${tojson(namespaceMetadata)}`);
            assert.eq(namespaceMetadata.placement[0].uuid,
                      namespaceMetadata.placement[1].uuid,
                      `Placement inconsistency detected. Details:  ${tojson(namespaceMetadata)}`);
            assert.sameMembers(
                namespaceMetadata.placement[0].shards,
                namespaceMetadata.placement[1].shards,
                `Placement inconsistency detected. Details:  ${tojson(namespaceMetadata)}`);
        });
    };

    const run = (mongos) => {
        try {
            jsTest.log('Checking routing table consistency');

            // Group docs in config.chunks by coll UUID (sorting by minKey), then join with docs in
            // config.collections.
            const testCollectionsWithRoutingTable = fetchRoutingTableData(mongos);

            testCollectionsWithRoutingTable.forEach(function(collData) {
                assert.eq(
                    1,
                    collData.details.length,
                    `There are entries in config.chunks which are either not linked to a collection or are linked to more than one collection! Details: ${
                        tojson(collData)}`);

                assert(checkCollRoutingTable(
                           collData.details[0]._id, collData.details[0].key, collData.routingTable),
                       `Corrupted routing table detected for ${collData._id}! Details: ${
                           tojson(collData)}`);
            });
            jsTest.log('Routing table consistency check completed');
        } catch (e) {
            if (e.code !== ErrorCodes.Unauthorized) {
                throw e;
            }
            jsTest.log(
                'Skipping check of routing table consistency - access to admin collections is not authorized');
        }

        try {
            jsTest.log('Checking consistency of config.placementHistory against the routing table');
            checkHistoricalPlacementMetadataConsistency(mongos);
            jsTest.log('config.placementHistory consistency check completed');

        } catch (e) {
            if (e.code !== ErrorCodes.Unauthorized) {
                throw e;
            }
            jsTest.log(
                'Skipping consistency check of config.placementHistory - access to admin collections is not authorized');
        }
    };

    return {
        run: run,
    };
})();
