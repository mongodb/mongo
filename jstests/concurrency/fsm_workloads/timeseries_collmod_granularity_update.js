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

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest' helpers.

var $config = (function() {
    const shardedCollName = i => `sharded_${i}`;
    const unshardedCollName = i => `unsharded_${i}`;
    const collCount = 50;
    const threadCount = 10;
    const collPerThread = collCount / threadCount;
    const timeField = 'time';
    const metaField = 'meta';
    const valueField = 'value';

    const secondsRoundingSeconds =
        TimeseriesTest.getBucketRoundingSecondsFromGranularity('seconds');
    const minutesRoundingSeconds =
        TimeseriesTest.getBucketRoundingSecondsFromGranularity('minutes');
    const hoursRoundingSeconds = TimeseriesTest.getBucketRoundingSecondsFromGranularity('hours');

    const data = {
        docCount: Array(collCount).fill(0),
        currentGranularityForEachColl: Array(collPerThread).fill('custom'),
        currentBucketRoundingSecondsForEachColl: Array(collPerThread).fill(1),
        isTimeseriesScalabilityImprovementsEnabled: false,
    };

    const states = {
        init: function(db, collName) {
            if (!this.isTimeseriesScalabilityImprovementsEnabled) {
                this.currentBucketRoundingSecondsForEachColl = Array(collPerThread).fill('seconds');
                this.currentBucketRoundingSecondsForEachColl = Array(collPerThread).fill(60);
            }
        },

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

        customBucketingUpdate: function(db, collName) {
            if (this.isTimeseriesScalabilityImprovementsEnabled) {
                const j = Random.randInt(collPerThread);
                const i = this.tid * collPerThread + j;

                const collGranularity = this.currentGranularityForEachColl[j];
                let nextBucketingValue = this.currentBucketRoundingSecondsForEachColl[j] + 1;
                if (collGranularity !== 'custom') {
                    // The inputs to bucketMaxSpanSeconds and bucketRoundingSeconds must be greater
                    // than their previous values, so we base the next bucketing value off of the
                    // bucketMaxSpanSeconds since it is the higher of the two parameters when
                    // derived from a granularity value.
                    nextBucketingValue =
                        TimeseriesTest.getBucketMaxSpanSecondsFromGranularity(collGranularity) + 1;
                }

                assert.commandWorked(db.runCommand({
                    collMod: shardedCollName(i),
                    timeseries: {
                        bucketMaxSpanSeconds: nextBucketingValue,
                        bucketRoundingSeconds: nextBucketingValue
                    }
                }));
                this.currentGranularityForEachColl[j] = 'custom';
                this.currentBucketRoundingSecondsForEachColl[j] = nextBucketingValue;
            }
        },

        granularityUpdate: function(db, collName) {
            // The granularity value can only be changed three times:
            // [custom]          -> seconds
            // [custom, seconds] -> minutes
            // [custom, minutes] -> hours
            // We try to maximise the number of updates that can succeed by allocating
            // '(collCount/threadCount)' collections for each tid.
            const j = Random.randInt(collPerThread);
            const collGranularity = this.currentGranularityForEachColl[j];
            const currentBucketRoundingSeconds = this.currentBucketRoundingSecondsForEachColl[j];

            if (collGranularity !== 'hours' &&
                currentBucketRoundingSeconds <= hoursRoundingSeconds) {
                let newGranularity = 'hours';
                let newRoundingSeconds = hoursRoundingSeconds;
                if (collGranularity === 'seconds') {
                    newGranularity = 'minutes';
                } else if (collGranularity === 'custom') {
                    if (currentBucketRoundingSeconds <= secondsRoundingSeconds) {
                        newGranularity = 'seconds';
                        newRoundingSeconds = secondsRoundingSeconds;
                    }
                    if (currentBucketRoundingSeconds <= minutesRoundingSeconds) {
                        newGranularity = 'minutes';
                        newRoundingSeconds = minutesRoundingSeconds;
                    }
                }

                const i = this.tid * collPerThread + j;
                assert.commandWorked(db.runCommand(
                    {collMod: shardedCollName(i), timeseries: {granularity: newGranularity}}));
                this.currentGranularityForEachColl[j] = newGranularity;
                this.currentBucketRoundingSecondsForEachColl[j] = newRoundingSeconds;
            }
        },
    };

    const setup = function(db, collName, cluster) {
        this.isTimeseriesScalabilityImprovementsEnabled =
            TimeseriesTest.timeseriesScalabilityImprovementsEnabled(db.getMongo());

        for (let i = 0; i < collCount; i++) {
            if (this.isTimeseriesScalabilityImprovementsEnabled) {
                const timeseriesOptions = {
                    timeField: timeField,
                    metaField: metaField,
                    bucketMaxSpanSeconds: 1,
                    bucketRoundingSeconds: 1
                };
                assert.commandWorked(
                    db.createCollection(unshardedCollName(i), {timeseries: timeseriesOptions}));
                assert.commandWorked(
                    db.createCollection(shardedCollName(i), {timeseries: timeseriesOptions}));
            } else {
                assert.commandWorked(db.createCollection(unshardedCollName(i)));
                assert.commandWorked(db.createCollection(shardedCollName(i), {
                    timeseries: {timeField: timeField, metaField: metaField, granularity: 'seconds'}
                }));
            }
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
        read: {read: 0.35, write: 0.35, customBucketingUpdate: 0.20, granularityUpdate: 0.1},
        write: {read: 0.35, write: 0.35, customBucketingUpdate: 0.20, granularityUpdate: 0.1},
        customBucketingUpdate: {read: 0.45, write: 0.45, customBucketingUpdate: 0.1},
        granularityUpdate:
            {read: 0.35, write: 0.35, customBucketingUpdate: 0.20, granularityUpdate: 0.1},
    };

    return {
        threadCount,
        iterations: 50,
        setup: setup,
        teardown: tearDown,
        states: states,
        transitions: transitions,
        data: data,
    };
})();
