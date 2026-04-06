/*
 * Arbitraries for generating timeseries measurement documents and streams of measurements.
 */

import {fc} from "jstests/third_party/fast_check/fc-4.6.0.js";

import {
    makeMetricArb,
    makeSensorDateMetricStreamArb,
    makeMetricStreamArb,
    makeMixedTypeMetricStreamArb,
    makeRunnyMetricStreamArb,
} from "jstests/write_path/timeseries/pbt/lib/metric_arbitraries.js";

/**
 * Make a single measurement document arbitrary.
 *
 * Shape:
 * {
 *   _id:         <uuid>,
 *   [timeFieldname]: <Date>,
 *   [metaFieldname]: <metaValue or generatedMetric>,
 *   <extraField1>: <metric (int/date/uuid)>,
 *   ...
 * }
 *
 * - If metaValue is provided (not undefined), metaField is constant(metaValue).
 * - If metaValue is undefined, we use the metric arbitrary to generate
 *   a meta value (for this doc) and use that.
 *
 * @param {string} timeFieldname
 * @param {string} metaFieldname
 * @param {*} [metaValue]         // optional
 * @param {number} [minFields=1]  // min # of extra metric fields
 * @param {number} [maxFields=5]  // max # of extra metric fields
 * @param {object} [options]
 * @param {object} [options.explicitArbitraries]
 * @param {object} [options.ranges]
 * @param {Array<string>} [options.types]
 * @param {Range} [options.ranges.intRange]
 * @param {Range} [options.ranges.dateRange]
 * @param {fc.Arbitrary<string>} [options.ranges.fieldNameArb]
 *
 * @returns {fc.Arbitrary<Object>}
 */
export function makeMeasurementDocArb(
    timeFieldname,
    metaFieldname,
    metaValue,
    minFields = 1,
    maxFields = 5,
    options = {},
) {
    if (typeof timeFieldname !== "string" || timeFieldname.length === 0) {
        throw new Error("makeMeasurementDocArb: timeFieldname must be a non-empty string");
    }
    if (typeof metaFieldname !== "string" || metaFieldname.length === 0) {
        throw new Error("makeMeasurementDocArb: metaFieldname must be a non-empty string");
    }

    const {
        intRange,
        doubleRange,
        longRange,
        decimalRange,
        dateRange,
        fieldNameArb = fc.string({minLength: 1, maxLength: 8}),
    } = options.ranges ?? {};
    const explicitArbitraries = options.explicitArbitraries ?? {};

    const defaultDateMin = new Date("1970-01-01T00:00:00.000Z");
    const defaultDateMax = new Date("2038-01-19T03:14:07.000Z");

    const dateMin = dateRange?.min ?? defaultDateMin;
    const dateMax = dateRange?.max ?? defaultDateMax;

    // Special-field generators
    const timeArb = fc.date({min: dateMin, max: dateMax});

    // Parent metric arb used for meta (when not fixed) and for extra fields
    const parentMetricArb = makeMetricArb(options.types, {
        intRange,
        doubleRange,
        longRange,
        decimalRange,
        dateRange: {min: dateMin, max: dateMax},
    });

    // If metaValue is provided, fix it; otherwise use parentMetricArb
    const metaArb = metaValue !== undefined ? fc.constant(metaValue) : parentMetricArb;

    // Field names for extra fields must not collide with reserved ones
    const baseFieldNameArb = fieldNameArb.filter(
        (name) => !["_id", timeFieldname, metaFieldname, ...Object.keys(explicitArbitraries)].includes(name),
    );

    const fieldNamesArb = fc.array(baseFieldNameArb, {
        minLength: minFields,
        maxLength: maxFields,
    });

    return fieldNamesArb.chain((fieldNames) => {
        const recordSpec = {
            [timeFieldname]: timeArb,
            [metaFieldname]: metaArb,
        };
        for (const [fieldName, factory] of Object.entries(explicitArbitraries)) {
            recordSpec[fieldName] = factory();
        }

        for (const name of fieldNames) {
            recordSpec[name] = parentMetricArb;
        }

        return fc.record(recordSpec);
    });
}

/**
 * Make a stream (array) of measurement docs that all share the same set
 * of extra field names. Each doc has:
 *
 * {
 *   _id:         <uuid>,
 *   [timeFieldname]: <Date>,
 *   [metaFieldname]: <metaValue or generatedMetric>,   // constant per stream
 *   <extraField1>: <metric>,
 *   ...
 * }
 *
 * - If metaValue is provided (not undefined), all docs in the stream use that
 *   value for metaFieldname.
 * - If metaValue is undefined, we generate a single meta value from the
 *   parent metric arbitrary and use it for the entire stream.
 *
 * For extra field, this function uses `makeMetricStreamArb`:
 *   - First it picks `docCount` (number of docs).
 *   - Then, for each extra field name, it creates a metric stream with
 *     `minLength = maxLength = docCount`, guaranteeing the stream length
 *     matches the doc array length.
 *   - It then zips each stream index into the corresponding document field.
 *
 * @param {string} timeFieldname
 * @param {string} metaFieldname
 * @param {*} [metaValue]   // optional
 * @param {Object} [options]
 * @param {Object} [options.explicitArbitraries]
 * @param {Object} [options.ranges]
 * @param {Range} [options.ranges.intRange]
 * @param {Range} [options.ranges.dateRange]
 * @param {number} [options.ranges.minFields=1]   // number of extra metric fields
 * @param {number} [options.ranges.maxFields=5]
 * @param {number} [options.ranges.minDocs=0]
 * @param {number} [options.ranges.maxDocs=20]
 * @param {Array<string>} [options.types] // types of metrics to include, leave undefined/null for all
 * @param {number} [options.mixedSchemaChance=0.0]  // chance that a given field will have a mixed schema across the stream
 * @param {fc.Arbitrary<string>} [options.ranges.fieldNameArb]
 *
 * @returns {fc.Arbitrary<Object[]>}
 */
export function makeMeasurementDocStreamArb(timeFieldname, metaFieldname, metaValue, options = {}) {
    if (typeof timeFieldname !== "string" || timeFieldname.length === 0) {
        throw new Error("makeMeasurementDocStreamArb: timeFieldname must be a non-empty string");
    }
    if (typeof metaFieldname !== "string" || metaFieldname.length === 0) {
        throw new Error("makeMeasurementDocStreamArb: metaFieldname must be a non-empty string");
    }

    const mixedSchemaChance = options.mixedSchemaChance ?? 0.0;
    if (typeof mixedSchemaChance !== "number" || mixedSchemaChance < 0 || mixedSchemaChance > 1) {
        throw new Error("makeMeasurementDocStreamArb: mixedSchemaChance must be a number in [0, 1]");
    }

    const {
        intRange,
        doubleRange,
        longRange,
        decimalRange,
        dateRange,
        minFields = 1,
        maxFields = 5,
        minDocs = 0,
        maxDocs = 20,
        fieldNameArb = fc.string({minLength: 1, maxLength: 8}),
        timeBucketing = "hours",
    } = options.ranges ?? {};
    const runFrequency = options.runFrequency ?? 0;
    const explicitArbitraries = options.explicitArbitraries ?? {};

    const defaultDateMin = new Date("1970-01-01T00:00:00.000Z");
    const defaultDateMax = new Date("2038-01-19T03:14:07.000Z");

    const dateMin = dateRange?.min ?? defaultDateMin;
    const dateMax = dateRange?.max ?? defaultDateMax;

    const baseFieldNameArb = fieldNameArb.filter(
        (name) => !["_id", timeFieldname, metaFieldname, ...Object.keys(explicitArbitraries)].includes(name),
    );

    const fieldNamesArb = fc.array(baseFieldNameArb, {
        minLength: minFields,
        maxLength: maxFields,
    });

    const docCountArb = fc.integer({min: minDocs, max: maxDocs});

    // Parent metric arb for meta (when not fixed) and extra fields
    const parentMetricArb = makeMetricArb(options.types, {
        intRange,
        doubleRange,
        longRange,
        decimalRange,
        dateRange: {min: dateMin, max: dateMax},
    });

    // Arbitrary that picks the meta value used for this entire stream
    const metaValueArb = metaValue !== undefined ? fc.constant(metaValue) : parentMetricArb;

    const rf = Math.max(0, Math.min(1, Number(runFrequency)));
    const useRunnyMetricsArb = fc.double({min: 0, max: 1}).map((d) => d < rf);

    return fc.tuple(fieldNamesArb, docCountArb, metaValueArb).chain(([fieldNames, docCount, chosenMetaValue]) => {
        if (docCount === 0) {
            return fc.constant([]);
        }

        // Sensor-like increasing time stream to create meaningful bucket boundaries.
        const timeStreamArb = makeSensorDateMetricStreamArb(
            docCount,
            docCount,
            {dateRange: {min: dateMin, max: dateMax}},
            timeBucketing,
        );

        const metaStreamArb = fc.array(fc.constant(chosenMetaValue), {minLength: docCount, maxLength: docCount});

        // Extra fields: each gets a metric stream built via makeMetricStreamArb or makeRunnyMetricStreamArb. We set both minLength and maxLength
        // with minLength = maxLength = docCount, so every stream[i] is defined.
        let extraFieldStreamArbs = {};
        const extraFieldRanges = {
            intRange,
            doubleRange,
            longRange,
            decimalRange,
            dateRange: {min: dateMin, max: dateMax},
        };
        const extraFieldOptions = {
            ranges: extraFieldRanges,
            types: options.types,
        };

        // Runny overrides mixed type, this should be fine for our use cases
        for (const fieldName of fieldNames) {
            const plainArb = fc
                .double({min: 0, max: 1, noNaN: true})
                .chain((selector) =>
                    selector < mixedSchemaChance
                        ? makeMixedTypeMetricStreamArb(docCount, docCount, extraFieldOptions)
                        : makeMetricStreamArb(docCount, docCount, extraFieldOptions),
                );
            const runnyArb = makeRunnyMetricStreamArb(parentMetricArb, {minLength: docCount, maxLength: docCount});
            extraFieldStreamArbs[fieldName] = useRunnyMetricsArb.chain((useRunny) => (useRunny ? runnyArb : plainArb));
        }

        for (const [fieldName, factory] of Object.entries(explicitArbitraries)) {
            extraFieldStreamArbs[fieldName] = fc.array(factory(), {minLength: docCount, maxLength: docCount});
        }

        return fc
            .tuple(timeStreamArb, metaStreamArb, ...Object.values(extraFieldStreamArbs))
            .map(([timeStream, metaStream, ...extraFieldStream]) => {
                const docs = [];

                for (let i = 0; i < docCount; ++i) {
                    const doc = {[timeFieldname]: timeStream[i], [metaFieldname]: metaStream[i]};

                    Object.keys(extraFieldStreamArbs).forEach((name, idx) => {
                        doc[name] = extraFieldStream[idx][i];
                    });

                    docs.push(doc);
                }

                return docs;
            });
    });
}

/**
 * Simple defaults if you just want "some" ts-style docs.
 * (metaValue omitted -> meta chosen from metric arb per doc/stream)
 */

export const simpleMeasurementDocArb = makeMeasurementDocArb(
    "ts", // timeField
    "meta", // metaField
    undefined, // metaValue: generated by metric arb
    // minFields, maxFields default to 1 and 5
);

export const simpleMeasurementDocStreamArb = makeMeasurementDocStreamArb("ts", "meta", undefined);
