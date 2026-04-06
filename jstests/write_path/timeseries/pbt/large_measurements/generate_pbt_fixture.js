/**
 * Leverage mochalite to generate a describe block with necessary hooks for large measurement PBTs
 * This is needed to split up long-running test cases into separate files to minimize evergreen timeout.
 */

import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";

import {fc} from "jstests/third_party/fast_check/fc-4.6.0.js";

import {makeEmptyModel} from "jstests/write_path/timeseries/pbt/lib/command_grammar.js";
import {assertBelowBsonSizeLimit, assertCollectionsMatch} from "jstests/write_path/timeseries/pbt/lib/assertions.js";
import {getFcAssertArgs} from "jstests/write_path/timeseries/pbt/lib/fast_check_params.js";

const fcAssertArgs = getFcAssertArgs();

/**
 * The Fixture class provides a container for various stats, and maintains the functions for Mochalite and fast-check lifecycle hooks. Specific test case files should set up the fixture and run it by passing a programArbitrary as an argument to `run`.
 */
export class Fixture {
    constructor(db, ctrlCollName, tsCollName, metaField, timeField) {
        this.db = db;
        this.ctrlCollName = ctrlCollName;
        this.tsCollName = tsCollName;
        this.metaField = metaField;
        this.timeField = timeField;
        this.beforeHook();
        this.resetStats();
    }

    resetStats() {
        // Clean the stats
        this.stats = {
            commands: {},
            documentCount: 0,
            documentSizeMean: 0,
            documentSizeMax: 0,
        };
    }

    logStats() {
        jsTest.log.info({stats: this.stats});
    }

    // Create fresh collections for the property testing.
    beforeHook() {
        this.db[this.ctrlCollName].drop();
        this.db[this.tsCollName].drop();

        this.db.createCollection(this.ctrlCollName);
        this.db.createCollection(this.tsCollName, {timeseries: {timeField: this.timeField, metaField: this.metaField}});

        this.ctrlColl = this.db.getCollection(this.ctrlCollName);
        this.tsColl = this.db.getCollection(this.tsCollName);
    }

    // Aggregate collStats before the next run which will drop the collection.
    afterHook() {
        const pipeline = [
            {
                $collStats: {
                    storageStats: {},
                },
            },
            {
                $replaceRoot: {
                    newRoot: "$storageStats.timeseries",
                },
            },
        ];
        const timeseriesCollStats = this.tsColl.aggregate(pipeline).toArray();
        if (this.stats.timeseriesCollStats === undefined) {
            this.stats.timeseriesCollStats = timeseriesCollStats;
        } else {
            for (const i in timeseriesCollStats) {
                for (const [key, value] of Object.entries(timeseriesCollStats[i])) {
                    if (key === "avgBucketSize") {
                        const oldBucketCount = this.stats.timeseriesCollStats[i].bucketCount;
                        const oldAvgBucketSize = this.stats.timeseriesCollStats[i].avgBucketSize;
                        const newBucketCount = timeseriesCollStats[i].bucketCount;
                        const newAvgBucketSize = value * timeseriesCollStats[i].bucketCount;
                        const oldWeight = oldBucketCount / (newBucketCount + oldBucketCount);
                        this.stats.timeseriesCollStats[i].avgBucketSize =
                            oldAvgBucketSize * oldWeight + newAvgBucketSize * (1.0 - oldWeight);
                    } else if (key == "avgNumMeasurementsPerCommit") {
                        // no-op, handle separately
                    } else {
                        // All others are summed
                        this.stats.timeseriesCollStats[key] += value;
                    }
                    // Recompute this mean
                    this.stats.timeseriesCollStats[i].avgNumMeasurementsPerCommit =
                        this.stats.timeseriesCollStats[i].numMeasurementsCommitted /
                        this.stats.timeseriesCollStats[i].numCommits;
                }
            }
        }
    }

    commandMeasurementStatsReducer(accumulatedStats, command) {
        const commandName = command.cmd.constructor.name;
        const tallyDocumentStats = (doc) => {
            const bsonSize = Object.bsonsize(doc);
            const totalDocSize = accumulatedStats.documentSizeMean * accumulatedStats.documentCount;
            accumulatedStats.documentCount += 1;
            accumulatedStats.documentSizeMean = (totalDocSize + bsonSize) / accumulatedStats.documentCount;
            accumulatedStats.documentSizeMax = Math.max(bsonSize, accumulatedStats.documentSizeMax);
        };
        switch (commandName) {
            case "InsertCommand":
                tallyDocumentStats(command.cmd.doc);
                break;
            case "BatchInsertCommand":
                command.cmd.docs.forEach((doc) => {
                    tallyDocumentStats(doc);
                });
                break;
        }
        accumulatedStats.commands[commandName] = (accumulatedStats.commands[commandName] || 0) + 1;
        return accumulatedStats;
    }

    run(programArb, descriptor) {
        describe("Comparative PBT large measurement inserts to timeseries collections", () => {
            // Classify each document involved in a write, get the mean and max document size
            beforeEach(() => this.resetStats());
            afterEach(() => this.logStats());
            it(descriptor, () => {
                fc.assert(
                    fc
                        .property(programArb, (cmds) => {
                            const model = makeEmptyModel();
                            fc.modelRun(
                                () => ({model: model, real: {tsColl: this.tsColl, ctrlColl: this.ctrlColl}}),
                                cmds,
                            );
                            this.stats = cmds.commands.reduce(this.commandMeasurementStatsReducer, this.stats);
                            assertCollectionsMatch(this.tsColl, this.ctrlColl);
                            assertBelowBsonSizeLimit(this.tsColl);
                        })
                        .beforeEach(() => this.beforeHook())
                        .afterEach(() => this.afterHook()),
                    fcAssertArgs,
                );
            });
        });
    }
}
