'use strict';

load("jstests/libs/feature_flag_util.js");

var RoutingTableConsistencyChecker = (function() {
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

    const fetchRoutingTableData =
        (mongos) => {
            // Group docs in config.chunks by coll UUID (sorting by minKey), then join with docs in
            // config.collections.
            return mongos.getDB('config')
                                .chunks
                                .aggregate([
                                    {$sort: {min: 1}},
                                    {
                                        $group: {
                                            _id: '$uuid',
                                            routingTable: {$push: '$$ROOT'},
                                        }
                                    },
                                    {$lookup: {
                                        from: 'collections',
                                        localField: '_id',
                                        foreignField: 'uuid',
                                        as: 'details'
                                    },
                                    }
                                ]);
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
     * Reproduces the logic implemented in ShardingCatalogManager::initializePlacementHistory()
     * to compute the placement of each existing collection and database by reading the content of
     * - config.collections + config.chunks
     * - config.databases.
     * The output format follows the same schema of config.placementHistory; results are ordered by
     * namespace.
     **/
    const buildCurrentPlacementData = (mongos) => {
        const pipeline = [
            {
                $lookup: {
                from: "chunks",
                localField: "uuid",
                foreignField: "uuid",
                as: "timestampByShard",
                pipeline: [
                    {
                     $group: {
                        _id: "$shard",
                        value: {
                        $max: "$onCurrentShardSince"
                        }
                    }
                    }
                ],
                }
            },
            {
                $project: {
                _id: 0,
                nss: "$_id",
                shards: "$timestampByShard._id",
                uuid: 1,
                timestamp: {
                    $max: "$timestampByShard.value"
                },
                }
            },
            {
                $unionWith: {
                 coll: "databases",
                 pipeline: [
                    {
                    $project: {
                        _id: 0,
                        nss: "$_id",
                        shards: [
                        "$primary"
                        ],
                        timestamp: "$version.timestamp"
                    }
                    }
                ]
                }
            },
            {
                $sort: {
                    nss: 1
                }
            }
            ];

        return mongos.getDB('config').collections.aggregate(pipeline);
    };

    /**
     * Extracts from config.placementHistory the most recent document for each collection
     * and database. Results are ordered by namespace.
     */
    const getHistoricalPlacementData = (mongos, atClusterTime) => {
        const pipeline = [
            {
                $match: {
                    // Skip documents containing initialization metadata
                    nss: {$ne: "."},
                    timestamp: {$lte: atClusterTime}
                }
            },
            {
                $group: {
                    _id: "$nss",
                    placement: {$top: {output: "$$CURRENT", sortBy: {"timestamp": -1}}}
                }
            },
            // Disregard placement entries on dropped namespaces
            {$match: {"placement.shards": {$not: {$size: 0}}}},
            {$replaceRoot: {newRoot: "$placement"}},
            {$sort: {nss: 1}}
        ];
        return mongos.getDB('config').placementHistory.aggregate(pipeline);
    };

    const checkHistoricalPlacementMetadataConsistency = (mongos) => {
        const placementDataFromRoutingTable = buildCurrentPlacementData(mongos);
        const now = mongos.getDB('admin').runCommand({isMaster: 1}).operationTime;
        const historicalPlacementData = getHistoricalPlacementData(mongos, now);

        placementDataFromRoutingTable.forEach(function(nssPlacementFromRoutingTable) {
            assert(historicalPlacementData.hasNext(),
                   `Historical placement data on ${nssPlacementFromRoutingTable.nss} is missing`);
            const historicalNssPlacement = historicalPlacementData.next();
            assert.eq(nssPlacementFromRoutingTable.nss,
                      historicalNssPlacement.nss,
                      'Historical placement data does not contain the expected number of entries');
            assert.sameMembers(nssPlacementFromRoutingTable.shards,
                               historicalNssPlacement.shards,
                               `Inconsistent placement info detected: routing table ${
                                   tojson(nssPlacementFromRoutingTable)} VS placement history ${
                                   tojson(historicalNssPlacement)}`);

            assert.eq(nssPlacementFromRoutingTable.uuid,
                      historicalNssPlacement.uuid,
                      `Inconsistent placement info detected: routing table ${
                          tojson(nssPlacementFromRoutingTable)} VS placement history ${
                          tojson(historicalNssPlacement)}`);
            // Timestamps are not compared, since they are expected to diverge if a chunk
            // migration, collection rename or a movePrimary request have been executed during
            // the test.
        });

        if (historicalPlacementData.hasNext()) {
            assert(false,
                   `Unexpected historical placement entries: ${
                       tojson(historicalPlacementData.toArray())}`);
        }
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

        // TODO (SERVER-68217): Remove this try/catch block once 7.0 becomes last LTS.
        try {
            const historicalPlacementMetadataDataAvailable = FeatureFlagUtil.isPresentAndEnabled(
                mongos.getDB('config'), "HistoricalPlacementShardingCatalog");
            if (!historicalPlacementMetadataDataAvailable) {
                jsTest.log(
                    'Skipping consistency check of config.placementHistory: feature disabled');
                return;
            }
        } catch (err) {
            jsTest.log(`Skipping consistency check of config.placementHistory: "${err}"`);
            return;
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
