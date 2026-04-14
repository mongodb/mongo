/**
 * A property-based test that compares compatible command sequences between timeseries
 * and non-timeseries collections, exercising all bucket rollover conditions:
 *   kTimeForward   - measurement time exceeds bucket max span
 *   kTimeBackward  - measurement time precedes bucket start
 *   kCount         - bucket reaches gTimeseriesBucketMaxCount (1000) measurements
 *   kSize          - bucket exceeds gTimeseriesBucketMaxSize (125 KB)
 *   kSchemaChange  - field type changes within a bucket (mixed-schema)
 *   kCachePressure - depends on server memory state; not injectable at doc level
 *
 * @tags: [
 *   query_intensive_pbt,
 *   requires_timeseries,
 *   requires_getmore,
 *   not_allowed_with_signed_security_token,
 * ]
 */

import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {fc} from "jstests/third_party/fast_check/fc-4.6.0.js";

import {makeEmptyModel} from "jstests/write_path/timeseries/pbt/lib/command_grammar.js";
import {makeTimeseriesCommandSequenceArb} from "jstests/write_path/timeseries/pbt/lib/command_arbitraries.js";
import {assertCollectionValid, assertCollectionsMatch} from "jstests/write_path/timeseries/pbt/lib/assertions.js";
import {getFcParams, getFcAssertArgs} from "jstests/write_path/timeseries/pbt/lib/fast_check_params.js";
import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {kSizeRolloverFieldName} from "jstests/write_path/timeseries/pbt/lib/measurement_arbitraries.js";

const fcParams = getFcParams();
const fcAssertArgs = getFcAssertArgs();

const ctrlCollName = jsTestName() + "_control";
const tsCollName = jsTestName() + "_timeseries";
const timeField = "ts";
const metaField = "meta";
const metaValue = "metavalue";

// Bucket span for the default "hours" granularity (3600 s).
const kBucketSpanSeconds = 3600;

function classifyValueType(value) {
    if (value === null) {
        return "null";
    }
    if (value === undefined) {
        return "undefined";
    }
    if (value instanceof Date) {
        return "date";
    }
    if (typeof value === "number") {
        return "double";
    }
    if (typeof value === "string") {
        return "string";
    }
    if (typeof value === "boolean") {
        return "bool";
    }
    if (value instanceof Timestamp) {
        return "timestamp";
    }
    if (value instanceof NumberLong) {
        return "long";
    }
    if (value instanceof NumberDecimal) {
        return "decimal";
    }
    if (value instanceof ObjectId) {
        return "objectId";
    }
    if (typeof BinData !== "undefined" && value instanceof BinData) {
        return "binData";
    }
    if (typeof DBPointer !== "undefined" && value instanceof DBPointer) {
        return "dbPointer";
    }
    if (typeof Code !== "undefined" && value instanceof Code) {
        return "javascript";
    }
    return typeof value;
}

function makeOrderedTypeKey(typeSet) {
    return Array.from(typeSet).sort().join("|");
}

function updateGeneratedFieldStreamStats(accumulatedStats, docs) {
    const fieldTypeSets = {};

    for (const doc of docs) {
        for (const [fieldName, value] of Object.entries(doc)) {
            if ([timeField, metaField].includes(fieldName)) {
                continue;
            }
            fieldTypeSets[fieldName] = fieldTypeSets[fieldName] || new Set();
            fieldTypeSets[fieldName].add(classifyValueType(value));
        }
    }

    for (const typeSet of Object.values(fieldTypeSets)) {
        if (typeSet.size > 1) {
            accumulatedStats.fieldStreamsGenerated.mixedTypeFields += 1;
            const typeKey = makeOrderedTypeKey(typeSet);
            accumulatedStats.insertedTypeCombinations[typeKey] =
                (accumulatedStats.insertedTypeCombinations[typeKey] || 0) + 1;
        } else {
            accumulatedStats.fieldStreamsGenerated.singleTypeFields += 1;
        }
    }

    return accumulatedStats;
}

function updateFinalCollectionFieldTypeStats(accumulatedStats, docs) {
    const fieldTypeSets = {};

    for (const doc of docs) {
        for (const [fieldName, value] of Object.entries(doc)) {
            if ([timeField, metaField].includes(fieldName)) {
                continue;
            }
            fieldTypeSets[fieldName] = fieldTypeSets[fieldName] || new Set();
            fieldTypeSets[fieldName].add(classifyValueType(value));
        }
    }

    for (const typeSet of Object.values(fieldTypeSets)) {
        if (typeSet.size > 1) {
            accumulatedStats.finalCollectionFieldTypes.mixedTypeFields += 1;
        } else {
            accumulatedStats.finalCollectionFieldTypes.singleTypeFields += 1;
        }
    }

    return accumulatedStats;
}

/**
 * Classify which rollover conditions are present in a batch of generated docs.
 * Returns counts of injected condition instances for statistics.
 */
function updateRolloverStats(accumulatedStats, docs) {
    if (docs.length === 0) return accumulatedStats;

    const timestamps = docs.map((d) => d[timeField].getTime());
    const minTs = Math.min(...timestamps);

    // kTimeForward: any consecutive gap > kBucketSpanSeconds * 1000 ms
    for (let i = 1; i < timestamps.length; i++) {
        if (timestamps[i] - timestamps[i - 1] > kBucketSpanSeconds * 1000) {
            accumulatedStats.rollover.timeForwardGaps += 1;
        }
    }

    // kTimeBackward: any timestamp strictly before the batch minimum
    // (detect docs where timestamp < minTs, i.e. injected backward timestamps)
    for (let i = 0; i < timestamps.length; i++) {
        if (timestamps[i] < minTs) {
            accumulatedStats.rollover.timeBackwardDocs += 1;
        }
    }

    // kSize: presence of the large-payload sentinel field
    for (const doc of docs) {
        if (Object.prototype.hasOwnProperty.call(doc, kSizeRolloverFieldName)) {
            accumulatedStats.rollover.sizeLargeDocs += 1;
        }
    }

    // kCount: batch large enough to trigger count rollover on its own
    if (docs.length > 1000) {
        accumulatedStats.rollover.countRolloverBatches += 1;
    }

    return accumulatedStats;
}

function makeEmptyStats() {
    return {
        commands: {},
        fieldStreamsGenerated: {
            mixedTypeFields: 0,
            singleTypeFields: 0,
        },
        insertedTypeCombinations: {},
        finalCollectionFieldTypes: {
            mixedTypeFields: 0,
            singleTypeFields: 0,
        },
        rollover: {
            timeForwardGaps: 0,
            timeBackwardDocs: 0,
            sizeLargeDocs: 0,
            countRolloverBatches: 0,
        },
    };
}

describe("Comparative PBT for timeseries bucket rollover conditions", () => {
    let tsColl;
    let ctrlColl;
    let bucketColl;
    let stats;

    const beforeHook = () => {
        db[ctrlCollName].drop();
        db[tsCollName].drop();

        db.createCollection(ctrlCollName);
        db.createCollection(tsCollName, {timeseries: {timeField, metaField}});

        ctrlColl = db.getCollection(ctrlCollName);
        tsColl = db.getCollection(tsCollName);
        bucketColl = getTimeseriesCollForRawOps(tsColl.getDB(), tsColl);
    };

    beforeEach(function () {
        stats = makeEmptyStats();
    });

    afterEach(function () {
        jsTest.log.info({
            "Arbitrary generation stats for this rollover PBT run": stats,
        });
    });

    const commandStatsReducer = (accumulatedStats, command) => {
        const commandName = command.cmd.constructor.name;
        accumulatedStats.commands[commandName] = (accumulatedStats.commands[commandName] || 0) + 1;

        if (commandName === "BatchInsertCommand") {
            accumulatedStats = updateGeneratedFieldStreamStats(accumulatedStats, command.cmd.docs);
            accumulatedStats = updateRolloverStats(accumulatedStats, command.cmd.docs);
        }

        return accumulatedStats;
    };

    /**
     * Core helper: run a property-based test with the given command-sequence arbitrary.
     * Asserts that tsColl and ctrlColl stay in sync and the TS collection remains valid.
     */
    function runRolloverPbt(programArb, assertArgs) {
        fc.assert(
            fc
                .property(programArb, (cmds) => {
                    const model = makeEmptyModel(ctrlColl, bucketColl);

                    fc.modelRun(() => ({model, real: {tsColl, ctrlColl}}), cmds);
                    assertCollectionsMatch(tsColl, ctrlColl);
                    assertCollectionValid(tsColl);

                    stats = cmds.commands.reduce(commandStatsReducer, stats);
                    stats = updateFinalCollectionFieldTypeStats(stats, tsColl.find().toArray());
                })
                .beforeEach(beforeHook),
            assertArgs ?? fcAssertArgs,
        );
    }

    // ---- Individual rollover condition tests ------------------------------------

    it("keeps tsColl and ctrlColl in sync under mixed-schema field streams (kSchemaChange)", () => {
        runRolloverPbt(
            makeTimeseriesCommandSequenceArb(
                fcParams.minCommands || 1,
                fcParams.maxCommands || 30,
                timeField,
                metaField,
                metaValue,
                1, // minFields
                3, // maxFields
                0, // minDocs
                50, // maxDocs
                {mixedSchemaChance: 0.5},
                undefined,
                fcParams.replayPath,
            ),
        );
    });

    it("keeps tsColl and ctrlColl in sync under time-forward rollover (kTimeForward)", () => {
        runRolloverPbt(
            makeTimeseriesCommandSequenceArb(
                fcParams.minCommands || 1,
                fcParams.maxCommands || 30,
                timeField,
                metaField,
                metaValue,
                1, // minFields
                3, // maxFields
                0, // minDocs
                50, // maxDocs
                {
                    rolloverConditions: {timeForwardChance: 0.3},
                    bucketSpanSeconds: kBucketSpanSeconds,
                },
                undefined,
                fcParams.replayPath,
            ),
        );
    });

    it("keeps tsColl and ctrlColl in sync under time-backward rollover (kTimeBackward)", () => {
        runRolloverPbt(
            makeTimeseriesCommandSequenceArb(
                fcParams.minCommands || 1,
                fcParams.maxCommands || 30,
                timeField,
                metaField,
                metaValue,
                1, // minFields
                3, // maxFields
                1, // minDocs (need at least one doc to inject a backward timestamp)
                50, // maxDocs
                {
                    rolloverConditions: {timeBackwardChance: 0.3},
                    bucketSpanSeconds: kBucketSpanSeconds,
                },
                undefined,
                fcParams.replayPath,
            ),
        );
    });

    it("keeps tsColl and ctrlColl in sync under size-based rollover (kSize)", () => {
        runRolloverPbt(
            makeTimeseriesCommandSequenceArb(
                fcParams.minCommands || 1,
                fcParams.maxCommands || 30,
                timeField,
                metaField,
                metaValue,
                1, // minFields
                3, // maxFields
                0, // minDocs
                50, // maxDocs
                {rolloverConditions: {sizeRolloverChance: 0.3}},
                undefined,
                fcParams.replayPath,
            ),
        );
    });

    it("keeps tsColl and ctrlColl in sync under count-based rollover (kCount)", () => {
        // Each batch contains >= 1001 docs so every batch causes at least one
        // kCount rollover.  Reduce numRuns to keep runtime reasonable.
        const countAssertArgs = {
            ...fcAssertArgs,
            numRuns: Math.max(1, Math.min(fcAssertArgs.numRuns, 3)),
        };
        runRolloverPbt(
            makeTimeseriesCommandSequenceArb(
                1, // minCommands
                2, // maxCommands - fewer commands since each batch is large
                timeField,
                metaField,
                metaValue,
                1, // minFields
                3, // maxFields
                0, // minDocs (overridden to 1001 by countRollover)
                50, // maxDocs (overridden to 1010 by countRollover)
                {rolloverConditions: {countRollover: true}},
                undefined,
                fcParams.replayPath,
            ),
            countAssertArgs,
        );
    });

    // ---- Comprehensive test: all conditions active simultaneously ----------------

    it("keeps tsColl and ctrlColl in sync with all rollover conditions active", () => {
        runRolloverPbt(
            makeTimeseriesCommandSequenceArb(
                fcParams.minCommands || 1,
                fcParams.maxCommands || 30,
                timeField,
                metaField,
                metaValue,
                1, // minFields
                3, // maxFields
                0, // minDocs
                50, // maxDocs
                {
                    mixedSchemaChance: 0.25,
                    rolloverConditions: {
                        timeForwardChance: 0.2,
                        timeBackwardChance: 0.15,
                        sizeRolloverChance: 0.15,
                        // countRollover omitted: combining 1001-doc batches with the other
                        // conditions is very expensive; kCount is covered by its own test above.
                    },
                    bucketSpanSeconds: kBucketSpanSeconds,
                },
                undefined,
                fcParams.replayPath,
            ),
        );
    });
});
