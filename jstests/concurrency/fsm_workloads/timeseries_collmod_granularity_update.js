'use strict';

/**
 * Tests read and write operations concurrent with granularity updates on sharded time-series
 * collection.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_fcv_60,
 *   does_not_support_transactions,
 * ]
 */

var $config = (function() {
    const shardedCollName = i => `sharded_${i}`;
    const unshardedCollName = i => `unsharded_${i}`;
    const collCount = 50;
    const threadCount = 10;
    const collPerThread = collCount / threadCount;
    const timeField = 'time';
    const metaField = 'meta';
    const valueField = 'value';

    const data = {
        docCount: Array(collCount).fill(0),
        currentGranularityLevelForEachColl: Array(collPerThread).fill(0),
    };

    const states = {
        init: function(db, collName) {},

        read: function(db, collName) {
            const i = Random.randInt(collCount);
            assert.eq(this.docCount[i],
                      db[shardedCollName(i)].countDocuments({[valueField]: this.tid}));
        },

        write: function(db, collName) {
            const i = Random.randInt(collCount);
            const time = new Date(2021,
                                  Random.randInt(12),
                                  Random.randInt(28) + 1,
                                  Random.randInt(24),
                                  Random.randInt(60),
                                  Random.randInt(60));
            assert.commandWorked(
                db[shardedCollName(i)].insert({[timeField]: time, [valueField]: this.tid}));
            assert.commandWorked(
                db[unshardedCollName(i)].insert({[timeField]: time, [valueField]: this.tid}));
            this.docCount[i]++;
        },

        granularityUpdate: function(db, collName) {
            // The granularity value can only be changed twice. From seconds -> minutes and minutes
            // -> hours. We try to maximise the number of updates that can succeed by allocating
            // '(collCount/threadCount)' collections for each tid.
            const j = Random.randInt(collPerThread);
            if (this.currentGranularityLevelForEachColl[j] < 2) {
                let granularity;
                if (this.currentGranularityLevelForEachColl[j] === 0) {
                    granularity = 'minutes';
                } else if (this.currentGranularityLevelForEachColl[j] === 1) {
                    granularity = 'hours';
                }
                const i = this.tid * collPerThread + j;
                assert.commandWorked(
                    db.runCommand({collMod: shardedCollName(i), timeseries: {granularity}}));
                this.currentGranularityLevelForEachColl[j]++;
            }
        },
    };

    const setup = function(db, collName, cluster) {
        for (let i = 0; i < collCount; i++) {
            assert.commandWorked(db.createCollection(unshardedCollName(i)));
            assert.commandWorked(db.createCollection(shardedCollName(i), {
                timeseries: {timeField: timeField, metaField: metaField, granularity: 'seconds'}
            }));
            cluster.shardCollection(db[shardedCollName(i)], {[timeField]: 1}, false);
        }
    };

    const tearDown = function(db, collName) {
        for (let i = 0; i < collCount; i++) {
            const unshardedDocCount = db[unshardedCollName(i)].countDocuments({});
            const shardedDocCount = db[shardedCollName(i)].countDocuments({});
            assert.eq(unshardedDocCount, shardedDocCount);
        }
    };

    const transitions = {
        init: {write: 1},
        read: {read: 0.45, write: 0.45, granularityUpdate: 0.1},
        write: {read: 0.45, write: 0.45, granularityUpdate: 0.1},
        granularityUpdate: {read: 0.45, write: 0.45, granularityUpdate: 0.1},
    };

    return {
        threadCount,
        iterations: 100,
        setup: setup,
        teardown: tearDown,
        states: states,
        transitions: transitions,
        data: data,
    };
})();
