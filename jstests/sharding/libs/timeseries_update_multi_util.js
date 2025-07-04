/**
 * Helpers for testing timeseries multi updates.
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

export const TimeseriesMultiUpdateUtil = (function() {
    const timeField = 'time';
    const metaField = 'hostid';

    // The split point between two shards. This value guarantees that generated time values do not
    // fall on this boundary.
    const splitTimePointBetweenTwoShards = ISODate("2001-06-30");
    const numOfDocs = 4;

    function generateTimeValue(index) {
        return ISODate(`${2000 + index}-01-01`);
    }

    const collectionConfigurations = {
        // Shard key only on meta field/subfields.
        metaShardKey: {
            metaGenerator: (id => id),
            shardKey: {[metaField]: 1},
            splitPoint: {meta: 2},
        },
        metaShardKeyString: {
            metaGenerator: (id => `string:${id}`),
            shardKey: {[metaField]: 1},
            splitPoint: {meta: `string:2`},
        },
        metaObjectShardKey: {
            metaGenerator: (index => ({a: index})),
            shardKey: {[metaField]: 1},
            splitPoint: {meta: {a: 2}},
        },
        metaSubFieldShardKey: {
            metaGenerator: (index => ({a: index})),
            shardKey: {[metaField + '.a']: 1},
            splitPoint: {'meta.a': 2},
        },

        // Shard key on time field.
        timeShardKey: {
            shardKey: {[timeField]: 1},
            splitPoint: {[`control.min.${timeField}`]: splitTimePointBetweenTwoShards},
        },

        // Shard key on both meta and time field.
        metaTimeShardKey: {
            metaGenerator: (id => id),
            shardKey: {[metaField]: 1, [timeField]: 1},
            splitPoint: {meta: 2, [`control.min.${timeField}`]: splitTimePointBetweenTwoShards},
        },
        metaObjectTimeShardKey: {
            metaGenerator: (index => ({a: index})),
            shardKey: {[metaField]: 1, [timeField]: 1},
            splitPoint:
                {meta: {a: 2}, [`control.min.${timeField}`]: splitTimePointBetweenTwoShards},
        },
        metaSubFieldTimeShardKey: {
            metaGenerator: (index => ({a: index})),
            shardKey: {[metaField + '.a']: 1, [timeField]: 1},
            splitPoint: {'meta.a': 2, [`control.min.${timeField}`]: splitTimePointBetweenTwoShards},
        },
    };

    function generateDocsForTestCase(collConfig) {
        const documents = TimeseriesTest.generateHosts(numOfDocs);
        for (let i = 0; i < numOfDocs; i++) {
            documents[i]._id = i;
            if (collConfig.metaGenerator) {
                documents[i][metaField] = collConfig.metaGenerator(i);
            }
            documents[i][timeField] = generateTimeValue(i);
            documents[i].f = i;
            documents[i].stringField = "testString";
        }
        return documents;
    }

    function prepareShardedTimeseriesCollection(
        mongos, shardingTest, db, collName, collConfig, insertFn) {
        // Ensures that the collection does not exist.
        const coll = db.getCollection(collName);
        coll.drop();

        // Creates timeseries collection.
        const tsOptions = {timeField: timeField};
        const hasMetaField = !!collConfig.metaGenerator;
        if (hasMetaField) {
            tsOptions.metaField = metaField;
        }
        assert.commandWorked(db.createCollection(collName, {timeseries: tsOptions}));

        // Shards timeseries collection.
        assert.commandWorked(coll.createIndex(collConfig.shardKey));
        assert.commandWorked(mongos.adminCommand({
            shardCollection: `${db.getName()}.${collName}`,
            key: collConfig.shardKey,
        }));

        // Inserts initial set of documents.
        const documents = generateDocsForTestCase(collConfig);
        assert.commandWorked(insertFn(coll, documents));

        // Manually splits the data into two chunks.
        assert.commandWorked(mongos.adminCommand({
            split: getTimeseriesCollForDDLOps(db, coll).getFullName(),
            middle: collConfig.splitPoint
        }));

        // Ensures that currently both chunks reside on the primary shard.
        let counts = shardingTest.chunkCounts(collName, db.getName());
        const primaryShard = shardingTest.getPrimaryShard(db.getName());
        assert.eq(2, counts[primaryShard.shardName], counts);

        // Moves one of the chunks into the second shard.
        const otherShard = shardingTest.getOther(primaryShard);
        assert.commandWorked(mongos.adminCommand({
            movechunk: getTimeseriesCollForDDLOps(db, coll).getFullName(),
            find: collConfig.splitPoint,
            to: otherShard.name,
            _waitForDelete: true
        }));

        // Ensures that each shard owns one chunk.
        counts = shardingTest.chunkCounts(collName, db.getName());
        assert.eq(1, counts[primaryShard.shardName], counts);
        assert.eq(1, counts[otherShard.shardName], counts);

        return [coll, documents];
    }

    return {
        timeField,
        metaField,
        collectionConfigurations,
        generateTimeValue,
        prepareShardedTimeseriesCollection,
    };
})();
