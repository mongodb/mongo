/*
 * Arbitraries for generating timeseries commands for property testing.
 */

import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

import {
    makeMeasurementDocArb,
    makeMeasurementDocStreamArb,
} from "jstests/libs/property_test_helpers/timeseries/measurement_arbitraries.js";
import {
    InsertCommand,
    BatchInsertCommand,
    DeleteByRandomIdCommand,
} from "jstests/libs/property_test_helpers/timeseries/command_grammar.js";

/**
 * Arbitrary for InsertCommand (single doc insert).
 *
 * Generates:
 *   new InsertCommand(doc)
 *
 * where `doc` is created by makeMeasurementDocArb.
 *
 * @param {string} [timeField='ts']
 * @param {string} [metaField='meta']
 * @param {*} [metaValue]                    // fixed meta or undefined
 * @param {number} [minFields=1]             // min extra metric fields per doc
 * @param {number} [maxFields=5]             // max extra metric fields per doc
 * @param {{intRange?: Range, dateRange?: Range}} [ranges]
 * @param {fc.Arbitrary<string>} [fieldNameArb=fc.string({minLength:1,maxLength:8})]
 *
 * @returns {fc.Arbitrary<InsertCommand>}
 */
export function makeInsertCommandArb(
    timeField = "ts",
    metaField = "meta",
    metaValue = undefined,
    minFields = 1,
    maxFields = 5,
    ranges = {},
    fieldNameArb = fc.string({minLength: 1, maxLength: 8}),
) {
    const measurementRanges = {
        intRange: ranges.intRange,
        dateRange: ranges.dateRange,
        fieldNameArb,
    };

    const docArb = makeMeasurementDocArb(timeField, metaField, metaValue, minFields, maxFields, measurementRanges);

    return docArb.map((doc) => new InsertCommand(doc));
}

/**
 * Arbitrary for BatchInsertCommand (insertMany).
 *
 * Generates:
 *   new BatchInsertCommand(docs)
 *
 * where `docs` is an array of measurement docs created by
 * makeMeasurementDocStreamArb.
 *
 * @param {string} [timeField='ts']
 * @param {string} [metaField='meta']
 * @param {*} [metaValue]                        // fixed meta or undefined
 * @param {number} [minFields=1]                 // min extra metric fields per doc
 * @param {number} [maxFields=5]                 // max extra metric fields per doc
 * @param {number} [minDocs=1]                   // min docs in the batch
 * @param {number} [maxDocs=500]                 // max docs in the batch
 * @param {{intRange?: Range, dateRange?: Range}} [ranges]
 * @param {fc.Arbitrary<string>} [fieldNameArb=fc.string({minLength:1,maxLength:8})]
 *
 * @returns {fc.Arbitrary<BatchInsertCommand>}
 */
export function makeBatchInsertCommandArb(
    timeField = "ts",
    metaField = "meta",
    metaValue = undefined,
    minFields = 1,
    maxFields = 5,
    minDocs = 1,
    maxDocs = 500,
    ranges = {},
    fieldNameArb = fc.string({minLength: 1, maxLength: 8}),
) {
    const measurementRanges = {
        intRange: ranges.intRange,
        dateRange: ranges.dateRange,
        minFields,
        maxFields,
        minDocs,
        maxDocs,
        fieldNameArb,
    };

    const docsArb = makeMeasurementDocStreamArb(timeField, metaField, metaValue, measurementRanges);

    return docsArb.map((docs) => new BatchInsertCommand(docs));
}

/**
 * Arbitrary for DeleteByRandomIdCommand.
 *
 * Generates:
 *   new DeleteByRandomIdCommand()
 *
 * The actual _id is chosen at run-time from the model by the command
 * itself; this arb just controls *when* such a delete appears.
 *
 * @returns {fc.Arbitrary<DeleteByRandomIdCommand>}
 */
export function makeDeleteByRandomIdCommandArb() {
    return fc.constant(new DeleteByRandomIdCommand());
}

/**
 * Arbitrary for a single timeseries command from the grammar:
 *
 *   - InsertCommand
 *   - BatchInsertCommand
 *   - DeleteByRandomIdCommand
 *
 * @param {string} [timeField='ts']
 * @param {string} [metaField='meta']
 * @param {*} [metaValue]
 * @param {number} [minFields=1]
 * @param {number} [maxFields=5]
 * @param {number} [minDocs=1]
 * @param {number} [maxDocs=500]
 * @param {{intRange?: Range, dateRange?: Range}} [ranges]
 * @param {fc.Arbitrary<string>} [fieldNameArb=fc.string({minLength:1,maxLength:8})]
 *
 * @returns {fc.Arbitrary<InsertCommand|BatchInsertCommand|DeleteByRandomIdCommand>}
 */
export function makeTimeseriesCommandArb(
    timeField = "ts",
    metaField = "meta",
    metaValue = undefined,
    minFields = 1,
    maxFields = 5,
    minDocs = 1,
    maxDocs = 500,
    ranges = {},
    fieldNameArb = fc.string({minLength: 1, maxLength: 8}),
) {
    const insertArb = makeInsertCommandArb(timeField, metaField, metaValue, minFields, maxFields, ranges, fieldNameArb);

    const batchInsertArb = makeBatchInsertCommandArb(
        timeField,
        metaField,
        metaValue,
        minFields,
        maxFields,
        minDocs,
        maxDocs,
        ranges,
        fieldNameArb,
    );

    const deleteArb = makeDeleteByRandomIdCommandArb();

    return fc.oneof(insertArb, batchInsertArb, deleteArb);
}

/**
 * Arbitrary for a sequence (array) of timeseries commands.
 *
 * @param {number} [minCommands=0]
 * @param {number} [maxCommands=20]
 * @param {string} [timeField='ts']
 * @param {string} [metaField='meta']
 * @param {*} [metaValue]
 * @param {number} [minFields=1]
 * @param {number} [maxFields=5]
 * @param {number} [minDocs=1]
 * @param {number} [maxDocs=500]
 * @param {{intRange?: Range, dateRange?: Range}} [ranges]
 * @param {fc.Arbitrary<string>} [fieldNameArb=fc.string({minLength:1,maxLength:8})]
 *
 * @returns {fc.Arbitrary<Array<InsertCommand|BatchInsertCommand|DeleteByRandomIdCommand>>}
 */
export function makeTimeseriesCommandSequenceArb(
    minCommands = 0,
    maxCommands = 20,
    timeField = "ts",
    metaField = "meta",
    metaValue = undefined,
    minFields = 1,
    maxFields = 5,
    minDocs = 1,
    maxDocs = 500,
    ranges = {},
    fieldNameArb = fc.string({minLength: 1, maxLength: 8}),
) {
    const cmdArb = makeTimeseriesCommandArb(
        timeField,
        metaField,
        metaValue,
        minFields,
        maxFields,
        minDocs,
        maxDocs,
        ranges,
        fieldNameArb,
    );

    return fc.array(cmdArb, {
        minLength: minCommands,
        maxLength: maxCommands,
    });
}
