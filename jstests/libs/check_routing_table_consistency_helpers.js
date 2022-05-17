'use strict';

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
        } catch (e) {
            if (e.code !== ErrorCodes.Unauthorized) {
                throw e;
            }
            jsTest.log(
                'Skipping check of routing table consistency - access to admin collections is not authorized');
        }
        jsTest.log('Routing table consistency check completed');
    };

    return {
        run: run,
    };
})();
