/*
 * Arbitraries for generating timeseries commands for property testing.
 */

import {fc} from "jstests/third_party/fast_check/fc-4.6.0.js";

import {
    makeMeasurementDocArb,
    makeMeasurementDocStreamArb,
} from "jstests/write_path/timeseries/pbt/lib/measurement_arbitraries.js";
import {
    InsertCommand,
    BatchInsertCommand,
    DeleteByFilterCommand,
    DeleteByRandomIdCommand,
    Filter,
    InsertOldBucketCommand,
    UpdateByFilterCommand,
} from "jstests/write_path/timeseries/pbt/lib/command_grammar.js";

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
    options = {},
    fieldNameArb = fc.string({minLength: 1, maxLength: 8}),
) {
    const measurementRanges = {
        intRange: options.ranges?.intRange,
        dateRange: options.ranges?.dateRange,
        fieldNameArb,
    };

    const docArb = makeMeasurementDocArb(timeField, metaField, metaValue, minFields, maxFields, {
        ...options,
        ...measurementRanges,
    });

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
 * @param {number} [options.mixedSchemaChance=0.0]  // chance that a given field will have a mixed schema across the stream
 * @param {Object} [options]
 * @param {Object} [options.explicitArbitraries] Object mapping field names to specific arbitrary factories to inject into the test suite
 * @param {{intRange?: Range, dateRange?: Range}} [options.ranges]
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
    options = {},
    fieldNameArb = fc.string({minLength: 1, maxLength: 8}),
) {
    options.ranges = {
        intRange: options.ranges?.intRange,
        dateRange: options.ranges?.dateRange,
        minFields,
        maxFields,
        minDocs,
        maxDocs,
        fieldNameArb,
    };

    const docsArb = makeMeasurementDocStreamArb(timeField, metaField, metaValue, {
        ...options,
    });

    return docsArb.map((docs) => new BatchInsertCommand(docs));
}

/**
 * Generates Filters that are model-dependent at runtime (via seed)
 *
 */
export function makeFilterArb(timeFieldname, metaFieldname, opts = {}) {
    const {
        maxDepth = 2,
        maxChildren = 3,
        // If provided, byFieldEqFromDoc's "allow" will be set to a subset of these.
        candidateFieldNames = undefined,
        // Default exclusions for byFieldEqFromDoc. (We include time/meta when provided.)
        defaultExclude = ["_id", timeFieldname, metaFieldname].filter(Boolean),
    } = opts;

    // Seed used to deterministically select from the model at runtime.
    const seedArb = fc.integer({min: -0x7fffffff, max: 0x7fffffff});

    // Optional time-range widening. Keep it modest to avoid always matching everything.
    const expandFactorArb = fc.oneof(
        fc.constant(0),
        fc.double({min: 0, max: 0.25, noNaN: true}),
        fc.double({min: 0.25, max: 1.0, noNaN: true}),
    );

    const leafArb = fc.oneof(
        fc.constant(Filter.matchAll()),
        seedArb.map((s) => Filter.byId(s)),
        seedArb.map((s) => Filter.byMetaEq(s)),
        fc.tuple(seedArb, expandFactorArb).map(([s, expandFactor]) => Filter.byTimeRange(s, {expandFactor})),
        // byFieldEqFromDoc:
        seedArb.chain((s) => {
            const excludeArb = fc.constant(defaultExclude);

            // If caller provides candidateFieldNames, pick a subset as an allow-list.
            // Otherwise omit allow entirely (Filter will use all fields minus excludes).
            const allowArb =
                Array.isArray(candidateFieldNames) && candidateFieldNames.length > 0
                    ? fc
                          .subarray(candidateFieldNames, {
                              minLength: 0,
                              maxLength: Math.min(candidateFieldNames.length, 6),
                          })
                          .map((allow) => ({allow}))
                    : fc.constant({});

            return fc.tuple(excludeArb, allowArb).map(([exclude, allowObj]) => {
                const params = {exclude, ...allowObj};
                return Filter.byFieldEqFromDoc(s, params);
            });
        }),
    );

    // Build a recursive arb for and/or compositions.
    return fc.letrec((tie) => ({
        filter: fc.oneof(
            leafArb,
            fc
                .tuple(fc.constantFrom("and", "or"), fc.array(tie("filter"), {minLength: 1, maxLength: maxChildren}))
                .map(([op, children]) => (op === "and" ? Filter.and(children) : Filter.or(children))),
        ),
    })).filter;
}

/**
 * Arbitrary for InsertOldBucketCommand.
 *
 * Generates:
 *   new InsertOldBucketCommand(pick, timeSeed, timeFieldname, metaFieldname)
 *
 * The actual bucket and timestamp are chosen at run-time from the model;
 * this arb only controls which bucket is targeted and where in its range the
 * new measurement lands.
 *
 * @param {string} timeFieldname
 * @param {string} metaFieldname
 * @returns {fc.Arbitrary<InsertOldBucketCommand>}
 */
export function makeInsertOldBucketCommandArb(timeFieldname, metaFieldname) {
    const pickArb = fc.integer({min: -0x7fffffff, max: 0x7fffffff});
    const timeSeedArb = fc.integer({min: -0x7fffffff, max: 0x7fffffff});
    return fc
        .tuple(pickArb, timeSeedArb)
        .map(([pick, timeSeed]) => new InsertOldBucketCommand(pick, timeSeed, timeFieldname, metaFieldname));
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
    // "pick" is an arbitrary integer used to provide randomness for id selection
    return fc.nat().map((pick) => new DeleteByRandomIdCommand(pick));
}

/**
 * Arbitrary for DeleteByFilterCommand.
 *
 * Generates:
 *   new DeleteByFilterCommand(filter, timeFieldname, metaFieldname)
 *
 * The actual filter is chosen at run-time from the model by the command
 * itself; this arb just controls *when* such a delete appears.
 *
 * @returns {fc.Arbitrary<DeleteByFilterCommand>}
 */
export function makeDeleteByFilterCommandArb(timeFieldname, metaFieldname, filterOpts = {}) {
    return makeFilterArb(timeFieldname, metaFieldname, filterOpts).map(
        (filter) => new DeleteByFilterCommand(filter, timeFieldname, metaFieldname),
    );
}

/**
 * Arbitrary for UpdateByFilterCommand.
 *
 * Generates:
 *   new UpdateByFilterCommand(filter, seed, timeFieldname, metaFieldname)
 *
 * The actual filter is chosen at run-time from the model by the command
 * itself; this arb just controls when such an update appears.
 *
 * @returns {fc.Arbitrary<UpdateByFilterCommand>}
 */
export function makeUpdateByFilterCommandArb(timeFieldname, metaFieldname, filterOpts = {}) {
    const seedArb = fc.integer({min: -0x7fffffff, max: 0x7fffffff});

    return fc
        .tuple(makeFilterArb(timeFieldname, metaFieldname, filterOpts), seedArb)
        .map(([filter, seed]) => new UpdateByFilterCommand(filter, seed, timeFieldname, metaFieldname));
}

/**
 * Arbitrary for a single timeseries command from the grammar:
 *
 *   - InsertCommand
 *   - BatchInsertCommand
 *   - DeleteByFilterCommand
 *   - UpdateByFilterCommand
 *
 * @param {string} [timeField='ts']
 * @param {string} [metaField='meta']
 * @param {*} [metaValue]
 * @param {number} [minFields=1]
 * @param {number} [maxFields=5]
 * @param {number} [minDocs=1]
 * @param {number} [maxDocs=500]
 * @param {Object} [options]
 * @param {Object} [options.explicitArbitraries] Object mapping field names to specific arbitrary factories to inject into the test suite
 * @param {{intRange?: Range, dateRange?: Range}} [options.ranges]
 * @param {fc.Arbitrary<string>} [fieldNameArb=fc.string({minLength:1,maxLength:8})]
 *
 * @returns {fc.Arbitrary<InsertCommand|BatchInsertCommand|DeleteByFilterCommand|UpdateByFilterCommand>}
 */
export function makeTimeseriesCommandArb(
    timeField = "ts",
    metaField = "meta",
    metaValue = undefined,
    minFields = 1,
    maxFields = 5,
    minDocs = 1,
    maxDocs = 500,
    options = {},
    fieldNameArb = fc.string({minLength: 1, maxLength: 8}),
) {
    const insertArb = makeInsertCommandArb(
        timeField,
        metaField,
        metaValue,
        minFields,
        maxFields,
        options,
        fieldNameArb,
    );

    const batchInsertArb = makeBatchInsertCommandArb(
        timeField,
        metaField,
        metaValue,
        minFields,
        maxFields,
        minDocs,
        maxDocs,
        options,
        fieldNameArb,
    );

    const deleteArb = makeDeleteByFilterCommandArb(timeField, metaField);

    const updateArb = makeUpdateByFilterCommandArb(timeField, metaField);

    return fc.oneof(insertArb, batchInsertArb, deleteArb, updateArb);
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
 * @param {Object} [options]
 * @param {Object} [options.explicitArbitraries] Object mapping field names to specific arbitrary factories to inject into the test suite
 * @param {Array<string>} [options.types] // types of metrics to include, leave undefined/null for all
 * @param {{intRange?: Range, dateRange?: Range}} [options.ranges]
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
    options = {},
    fieldNameArb = fc.string({minLength: 1, maxLength: 8}),
    replayPath = undefined,
) {
    const insertArb = makeInsertCommandArb(
        timeField,
        metaField,
        metaValue,
        minFields,
        maxFields,
        options,
        fieldNameArb,
    );

    const batchInsertArb = makeBatchInsertCommandArb(
        timeField,
        metaField,
        metaValue,
        minFields,
        maxFields,
        minDocs,
        maxDocs,
        options,
        fieldNameArb,
    );

    const deleteArb = makeDeleteByRandomIdCommandArb();

    return fc.commands([insertArb, batchInsertArb, deleteArb], {
        maxCommands,
        replayPath,
    });
}
