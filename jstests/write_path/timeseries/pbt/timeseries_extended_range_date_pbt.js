/**
 * A property-based test that compares compatible command sequences between timeseries
 * and non-timeseries collections, with timestamps that may exceed 32-bit second-granularity timestamps.
 *
 * @tags: [
 *   query_intensive_pbt,
 *   requires_timeseries,
 *   # Runs queries that may return many results, requiring getmores.
 *   requires_getmore,
 *   # This test runs commands that are not allowed with security token: setParameter.
 *   not_allowed_with_signed_security_token,
 * ]
 */

import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

import {makeEmptyModel} from "jstests/write_path/timeseries/pbt/lib/command_grammar.js";
import {makeTimeseriesCommandSequenceArb} from "jstests/write_path/timeseries/pbt/lib/command_arbitraries.js";
import {assertCollectionsMatch} from "jstests/write_path/timeseries/pbt/lib/assertions.js";
import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {getFcParams, getFcAssertArgs} from "jstests/write_path/timeseries/pbt/lib/fast_check_params.js";
const fcParams = getFcParams();
const fcAssertArgs = getFcAssertArgs();

const ctrlCollName = jsTestName() + "_control";
const tsCollName = jsTestName() + "_timeseries";
const timeField = "ts";
const metaField = "meta";

describe("Basic comparative PBT for timeseries inserts", () => {
    let tsColl;
    let ctrlColl;
    let bucketColl;
    let stats;

    const metaValue = "metavalue";

    // The ranges cover a few different types of extended ranges.
    // Extended ranges are defined as timestamps that cannot be
    // expressed with seconds granularity with a 32-bit integer
    // ranging from the Unix epoch to positive overflow in the year 2038.
    const epochStart = new Date(0);
    const epochEnd = new Date("2038-01-19T03:14:07Z");

    // This covers the entire range of ISO 8601 timestamps
    // Arbitraries generated from here may not cover dates
    // often used in typical workloads.
    const extendedDateRangeWide = {
        min: new Date("0000-01-01T00:00:00.000Z"),
        max: new Date("9999-12-31T23:59:59.999Z"),
    };

    // This covers a range of ~20 years before to ~20 years after.
    const extendedDateRangeNarrow = {
        min: new Date("1950-01-01T00:00:00.000Z"),
        max: new Date("2060-12-31T23:59:59.999Z"),
    };

    // The following ranges cover wide and narrow ranges around
    // the critical start and end dates for 32-bit epoch time.
    // They are intended to exercise inserts of documents with
    // mixed classification and potentially create buckets with
    // min/max values that span the critical dates.
    const extendedDateRangeNearEpochMinWide = {
        min: new Date("1969-12-15T00:00:00.000Z"),
        max: new Date("1970-01-15T00:00:00.000Z"),
    };
    const extendedDateRangeNearEpochMaxWide = {
        min: new Date("2038-01-01T00:00:00.000Z"),
        max: new Date("2038-02-01T00:00:00.000Z"),
    };
    const extendedDateRangeNearEpochMinNarrow = {
        min: new Date("1969-12-31T23:57:00.000Z"),
        max: new Date("1970-01-01T00:03:00.000Z"),
    };
    const extendedDateRangeNearEpochMaxNarrow = {
        min: new Date("2038-01-19T03:11:00.000Z"),
        max: new Date("2038-02-01T03:17:00.000Z"),
    };

    const extendedDateRanges = {
        extendedDateRangeNarrow,
        extendedDateRangeWide,
        extendedDateRangeNearEpochMinWide,
        extendedDateRangeNearEpochMaxWide,
        extendedDateRangeNearEpochMinNarrow,
        extendedDateRangeNearEpochMaxNarrow,
    };

    const beforeHook = () => {
        db[ctrlCollName].drop();
        db[tsCollName].drop();

        db.createCollection(ctrlCollName);
        db.createCollection(tsCollName, {timeseries: {timeField: timeField, metaField: metaField}});

        ctrlColl = db.getCollection(ctrlCollName);
        tsColl = db.getCollection(tsCollName);
        bucketColl = getTimeseriesCollForRawOps(tsColl.getDB(), tsColl);
    };

    // Zero out the arbitrary generation stats before exercising a range.
    beforeEach(function () {
        stats = {
            measurementsInserted: {
                "before epoch start": 0,
                "after epoch end": 0,
                "within epoch": 0,
            },
            commands: {}, // Inserted dynamically, the selection of commands may be changed outside of this file
            measurementsFinalQuery: {
                "before epoch start": 0,
                "after epoch end": 0,
                "within epoch": 0,
            },
            bucketsFinalQuery: {
                "spanning epoch start": 0,
                "spanning epoch end": 0,
                "no critical span": 0,
            },
        };
    });

    // Run the reducer to get stats regarding the arbitraries.
    // This is intended to check whether the arbitraries are generating values
    // that exercise important date values.
    // The stats include arbitraries generated from all runs in a given range.
    afterEach(function () {
        const percentages = {};
        for (const [statSliceName, statSlice] of Object.entries(stats)) {
            const sum = Object.values(statSlice).reduce((acc, val) => acc + val, 0);
            percentages[statSliceName] = Object.fromEntries(
                Object.entries(statSlice).map(([key, val]) => {
                    return [key, (100.0 * val) / sum];
                }),
            );
        }
        jsTest.log.info({"Arbitrary generation stats for this test run": {stats, percentages}});
    });

    const commandMeasurementStatsReducer = (accumulatedStats, command) => {
        const classifyMeasurement = (measurement) => {
            const label =
                measurement[timeField] > epochEnd
                    ? "after epoch end"
                    : measurement[timeField] < epochStart
                      ? "before epoch start"
                      : "within epoch";
            accumulatedStats.measurementsInserted[label] += 1;
        };

        // Classify each document involved in a write
        const commandName = command.cmd.constructor.name;
        switch (commandName) {
            case "InsertCommand":
                classifyMeasurement(command.cmd.doc);
                break;
            case "BatchInsertCommand":
                command.cmd.docs.forEach(classifyMeasurement);
                break;
        }

        // Track the count of each type of command
        stats.commands[commandName] = (stats.commands[commandName] || 0) + 1;

        return accumulatedStats;
    };

    const bucketCollectionQueryStatsReducer = (accumulatedStats, doc) => {
        const label =
            doc.control.min[timeField] < epochStart && doc.control.max[timeField] > epochStart
                ? "spanning epoch start"
                : doc.control.min[timeField] < epochEnd && doc.control.max[timeField] > epochEnd
                  ? "spanning epoch end"
                  : "no critical span";
        accumulatedStats.bucketsFinalQuery[label] += 1;
        return accumulatedStats;
    };

    const measurementsQueryStatsReducer = (accumulatedStats, measurement) => {
        const label =
            measurement[timeField] > epochEnd
                ? "after epoch end"
                : measurement[timeField] < epochStart
                  ? "before epoch start"
                  : "within epoch";
        accumulatedStats.measurementsFinalQuery[label] += 1;
        return accumulatedStats;
    };

    for (const [label, dateRange] of Object.entries(extendedDateRanges)) {
        const programArb = makeTimeseriesCommandSequenceArb(
            /* minCommands   */ fcParams.minCommands || 1,
            /* maxCommands   */ fcParams.maxCommands || 30,
            /* timeField     */ timeField,
            /* metaField     */ metaField,
            /* metaValue     */ metaValue,
            /* minFields     */ 1,
            /* maxFields     */ 3,
            /* minDocs       */ 0,
            /* maxDocs       */ 10,
            /* options       */ {ranges: {dateRange}}, // {intRange, dateRange} if you want to override
            /* fieldNameArb  */ undefined, // use default short-string field names
            /* replayPath    */ fcParams.replayPath,
        );
        it(`keeps tsColl and ctrlColl in sync under insert/batch-insert/delete: ${label}`, () => {
            fc.assert(
                fc
                    .property(programArb, (cmds) => {
                        const model = makeEmptyModel(ctrlColl, bucketColl);
                        fc.modelRun(() => ({model: model, real: {tsColl, ctrlColl}}), cmds);
                        assertCollectionsMatch(tsColl, ctrlColl);
                        stats = cmds.commands.reduce(commandMeasurementStatsReducer, stats);
                        stats = tsColl.find().toArray().reduce(measurementsQueryStatsReducer, stats);
                        stats = getTimeseriesCollForRawOps(tsColl.getDB(), tsColl)
                            .find()
                            .rawData()
                            .toArray()
                            .reduce(bucketCollectionQueryStatsReducer, stats);
                    })
                    .beforeEach(beforeHook),
                fcAssertArgs,
            );
        });
    }
});
