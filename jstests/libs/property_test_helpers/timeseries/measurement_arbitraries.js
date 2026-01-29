/*
 * Arbitraries for generating timeseries measurement documents and streams of measurements.
 */

import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

import {makeMetricArb, makeMetricStreamArb} from "jstests/libs/property_test_helpers/timeseries/metric_arbitraries.js";

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
 * @param {Object} [ranges]
 * @param {Range} [ranges.intRange]
 * @param {Range} [ranges.dateRange]
 * @param {fc.Arbitrary<string>} [ranges.fieldNameArb]
 *
 * @returns {fc.Arbitrary<Object>}
 */
export function makeMeasurementDocArb(
    timeFieldname,
    metaFieldname,
    metaValue,
    minFields = 1,
    maxFields = 5,
    ranges = {},
) {
    if (typeof timeFieldname !== "string" || timeFieldname.length === 0) {
        throw new Error("makeMeasurementDocArb: timeFieldname must be a non-empty string");
    }
    if (typeof metaFieldname !== "string" || metaFieldname.length === 0) {
        throw new Error("makeMeasurementDocArb: metaFieldname must be a non-empty string");
    }

    const {intRange, dateRange, fieldNameArb = fc.string({minLength: 1, maxLength: 8})} = ranges;

    const intMin = intRange?.min ?? -1000;
    const intMax = intRange?.max ?? 1000;

    const defaultDateMin = new Date("1970-01-01T00:00:00.000Z");
    const defaultDateMax = new Date("2038-01-19T03:14:07.000Z");

    const dateMin = dateRange?.min ?? defaultDateMin;
    const dateMax = dateRange?.max ?? defaultDateMax;

    // Special-field generators
    const idArb = fc.uuid();
    const timeArb = fc.date({min: dateMin, max: dateMax});

    // Parent metric arb used for meta (when not fixed) and for extra fields
    const parentMetricArb = makeMetricArb(["int", "date", "uuid"], {
        intRange: {min: intMin, max: intMax},
        dateRange: {min: dateMin, max: dateMax},
    });

    // If metaValue is provided, fix it; otherwise use parentMetricArb
    const metaArb = metaValue !== undefined ? fc.constant(metaValue) : parentMetricArb;

    // Field names for extra fields must not collide with reserved ones
    const baseFieldNameArb = fieldNameArb.filter(
        (name) => name !== "_id" && name !== timeFieldname && name !== metaFieldname,
    );

    const fieldNamesArb = fc.array(baseFieldNameArb, {
        minLength: minFields,
        maxLength: maxFields,
    });

    return fieldNamesArb.chain((fieldNames) => {
        const recordSpec = {
            _id: idArb,
        };

        recordSpec[timeFieldname] = timeArb;
        recordSpec[metaFieldname] = metaArb;

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
 * @param {Object} [ranges]
 * @param {Range} [ranges.intRange]
 * @param {Range} [ranges.dateRange]
 * @param {number} [ranges.minFields=1]   // number of extra metric fields
 * @param {number} [ranges.maxFields=5]
 * @param {number} [ranges.minDocs=0]
 * @param {number} [ranges.maxDocs=20]
 * @param {fc.Arbitrary<string>} [ranges.fieldNameArb]
 *
 * @returns {fc.Arbitrary<Object[]>}
 */
export function makeMeasurementDocStreamArb(timeFieldname, metaFieldname, metaValue, ranges = {}) {
    if (typeof timeFieldname !== "string" || timeFieldname.length === 0) {
        throw new Error("makeMeasurementDocStreamArb: timeFieldname must be a non-empty string");
    }
    if (typeof metaFieldname !== "string" || metaFieldname.length === 0) {
        throw new Error("makeMeasurementDocStreamArb: metaFieldname must be a non-empty string");
    }

    const {
        intRange,
        dateRange,
        minFields = 1,
        maxFields = 5,
        minDocs = 0,
        maxDocs = 20,
        fieldNameArb = fc.string({minLength: 1, maxLength: 8}),
    } = ranges;

    const intMin = intRange?.min ?? -1000;
    const intMax = intRange?.max ?? 1000;

    const defaultDateMin = new Date("1970-01-01T00:00:00.000Z");
    const defaultDateMax = new Date("2038-01-19T03:14:07.000Z");

    const dateMin = dateRange?.min ?? defaultDateMin;
    const dateMax = dateRange?.max ?? defaultDateMax;

    const baseFieldNameArb = fieldNameArb.filter(
        (name) => name !== "_id" && name !== timeFieldname && name !== metaFieldname,
    );

    const fieldNamesArb = fc.array(baseFieldNameArb, {
        minLength: minFields,
        maxLength: maxFields,
    });

    const docCountArb = fc.integer({min: minDocs, max: maxDocs});

    // Parent metric arb for meta (when not fixed) and extra fields
    const parentMetricArb = makeMetricArb(["int", "date", "uuid"], {
        intRange: {min: intMin, max: intMax},
        dateRange: {min: dateMin, max: dateMax},
    });

    // Arbitrary that picks the meta value used for this entire stream
    const metaValueArb = metaValue !== undefined ? fc.constant(metaValue) : parentMetricArb;

    return fc.tuple(fieldNamesArb, docCountArb, metaValueArb).chain(([fieldNames, docCount, chosenMetaValue]) => {
        if (docCount === 0) {
            return fc.constant([]);
        }

        // Streams for special fields (fixed length docCount)
        const idStreamArb = fc.array(fc.uuid(), {
            minLength: docCount,
            maxLength: docCount,
        });

        const timeStreamArb = fc.array(fc.date({min: dateMin, max: dateMax}), {
            minLength: docCount,
            maxLength: docCount,
        });

        const metaStreamArb = fc.array(fc.constant(chosenMetaValue), {minLength: docCount, maxLength: docCount});

        // Extra fields: each gets a metric stream built via makeMetricStreamArb
        // with minLength = maxLength = docCount, so every stream[i] is defined.
        const extraFieldStreamArbs = fieldNames.map(() =>
            makeMetricStreamArb(docCount, docCount, {
                intRange: {min: intMin, max: intMax},
                dateRange: {min: dateMin, max: dateMax},
            }),
        );

        return fc
            .tuple(idStreamArb, timeStreamArb, metaStreamArb, ...extraFieldStreamArbs)
            .map(([idStream, timeStream, metaStream, ...metricStreamsPerField]) => {
                const docs = [];

                for (let i = 0; i < docCount; ++i) {
                    const doc = {
                        _id: idStream[i],
                    };

                    doc[timeFieldname] = timeStream[i];
                    doc[metaFieldname] = metaStream[i];

                    for (let f = 0; f < fieldNames.length; ++f) {
                        const fieldName = fieldNames[f];
                        const streamForField = metricStreamsPerField[f];
                        doc[fieldName] = streamForField[i];
                    }

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
