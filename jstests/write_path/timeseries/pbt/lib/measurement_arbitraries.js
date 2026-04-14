/*
 * Arbitraries for generating timeseries measurement documents and streams of measurements.
 */

import {fc} from "jstests/third_party/fast_check/fc-4.6.0.js";

import {normalDistRealArb} from "jstests/write_path/timeseries/pbt/lib/arb_utils.js";
import {
    makeMetricArb,
    makeSensorDateMetricStreamArb,
    makeMetricStreamArb,
    makeMixedTypeMetricStreamArb,
    makeRunnyMetricStreamArb,
} from "jstests/write_path/timeseries/pbt/lib/metric_arbitraries.js";

// Rollover-condition constants

// Matches server-side gTimeseriesBucketMaxCount (timeseries.idl).
// When countRollover is enabled, streams are sized to exceed this limit.
export const kBucketMaxCount = 1000;

// Field name injected into documents when sizeRolloverChance fires.
// The double-underscore prefix and length (>8 chars) guarantee it never collides
// with the 1-8 char names produced by the default fieldNameArb.
export const kSizeRolloverFieldName = "__largePayloadField";

// Per-document payload size (bytes) used for size-rollover injection.
// Chosen to exceed gTimeseriesBucketMaxSize (128 000 B) on its own, so that a single
// injected document triggers the size check regardless of prior bucket contents.
// The value stays well below kLargeMeasurementsMaxBucketSize (12 MB)
// so the document is absorbed rather than causing a kSize/kCachePressure rollover.
const kSizeRolloverFieldBytes = 130_000;

// Pre-built once at module load; reused across every injected document.
const kSizeRolloverLargeValue = "x".repeat(kSizeRolloverFieldBytes);

/**
 * Build a time stream where some inter-document gaps are forced to exceed
 * `bucketSpanSeconds`, triggering a kTimeForward bucket rollover.
 *
 * For each gap between consecutive timestamps the caller decides (via `chance`)
 * whether to widen it to > bucketSpanMs.  Widening is cumulative so the
 * resulting stream stays monotonically non-decreasing.
 *
 * @param {number} docCount
 * @param {Date} dateMin
 * @param {Date} dateMax
 * @param {number} bucketSpanSeconds
 * @param {string|object} timeBucketing - forwarded to makeSensorDateMetricStreamArb
 * @param {number} chance - probability per gap of injecting a forward jump [0, 1]
 * @returns {fc.Arbitrary<Date[]>}
 */
function makeTimeForwardStreamArb(docCount, dateMin, dateMax, bucketSpanSeconds, timeBucketing, chance) {
    const bucketSpanMs = bucketSpanSeconds * 1_000;
    const baseArb = makeSensorDateMetricStreamArb(
        docCount,
        docCount,
        {dateRange: {min: dateMin, max: dateMax}},
        timeBucketing,
    );

    if (docCount <= 1) return baseArb;

    const numGaps = docCount - 1;
    const decisionsArb = fc.array(fc.double({min: 0, max: 1, noNaN: true}), {
        minLength: numGaps,
        maxLength: numGaps,
    });
    // Extra milliseconds added on top of the minimum needed to cross the boundary.
    const extraJumpArb = fc.array(fc.integer({min: 0, max: bucketSpanMs * 2}), {
        minLength: numGaps,
        maxLength: numGaps,
    });

    return fc.tuple(baseArb, decisionsArb, extraJumpArb).map(([stream, decisions, extras]) => {
        const result = [stream[0]];
        let cumulativeOffset = 0;

        for (let i = 1; i < docCount; i++) {
            const gap = stream[i].getTime() - stream[i - 1].getTime();
            if (decisions[i - 1] < chance) {
                // Force this gap to exceed the bucket span.
                cumulativeOffset += Math.max(0, bucketSpanMs + 1 - gap) + extras[i - 1];
            }
            const newMs = Math.min(stream[i].getTime() + cumulativeOffset, dateMax.getTime());
            result.push(new Date(newMs));
        }
        return result;
    });
}

/**
 * Post-process a docs array to inject:
 *   - kTimeBackward: replace some timestamps with values before the stream minimum
 *   - kSize:         add a large field to some documents
 *
 * Returns an fc.Arbitrary so that the random selections participate in
 * fast-check's shrinking.
 *
 * @param {Object[]} docs
 * @param {string} timeFieldname
 * @param {number} timeBackwardChance - per-doc probability [0, 1]
 * @param {number} sizeRolloverChance - per-doc probability [0, 1]
 * @param {Date} dateMin
 * @returns {fc.Arbitrary<Object[]>}
 */
function applyRolloverInjections(docs, timeFieldname, timeBackwardChance, sizeRolloverChance, dateMin) {
    if (docs.length === 0 || (timeBackwardChance <= 0 && sizeRolloverChance <= 0)) {
        return fc.constant(docs);
    }

    const n = docs.length;
    const minTimestampMs = docs.reduce((min, d) => Math.min(min, d[timeFieldname].getTime()), Infinity);

    // Backward-timestamp decisions + offset magnitudes.
    const backDecisionsArb =
        timeBackwardChance > 0
            ? fc.array(normalDistRealArb(0, 1), {minLength: n, maxLength: n})
            : fc.constant(new Array(n).fill(1.0));
    const backOffsetArb =
        timeBackwardChance > 0
            ? fc.array(fc.integer({min: 3_600_000, max: 86_400_000 * 7}), {minLength: n, maxLength: n})
            : fc.constant(new Array(n).fill(0));

    // Size-field decisions.
    const sizeDecisionsArb =
        sizeRolloverChance > 0
            ? fc.array(fc.double({min: 0, max: 1, noNaN: true}), {minLength: n, maxLength: n})
            : fc.constant(new Array(n).fill(1.0));

    return fc
        .tuple(backDecisionsArb, backOffsetArb, sizeDecisionsArb)
        .map(([backDecisions, backOffsets, sizeDecisions]) => {
            return docs.map((doc, i) => {
                const result = {...doc};
                if (backDecisions[i] < timeBackwardChance) {
                    const backMs = Math.max(dateMin.getTime(), minTimestampMs - backOffsets[i]);
                    result[timeFieldname] = new Date(backMs);
                }
                if (sizeDecisions[i] < sizeRolloverChance) {
                    result[kSizeRolloverFieldName] = kSizeRolloverLargeValue;
                }
                return result;
            });
        });
}

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
 * @param {number} [options.newFieldFrequency=0.1]   // per-doc probability that a new field is added to the schema going forward
 * @param {fc.Arbitrary<string>} [options.ranges.fieldNameArb]
 * @param {Object} [options.rolloverConditions]  // composable bucket-rollover injections
 * @param {number} [options.rolloverConditions.timeForwardChance=0.0]
 *   Per-step probability [0,1] of injecting a gap that exceeds bucketSpanSeconds,
 *   triggering a kTimeForward bucket rollover.
 * @param {number} [options.rolloverConditions.timeBackwardChance=0.0]
 *   Per-doc probability [0,1] of replacing a timestamp with one earlier than the
 *   stream minimum, triggering a kTimeBackward bucket rollover.
 * @param {boolean} [options.rolloverConditions.countRollover=false]
 *   When true, forces minDocs >= kBucketMaxCount+1 (1001) so that each batch
 *   is large enough to trigger a kCount bucket rollover.
 * @param {number} [options.rolloverConditions.sizeRolloverChance=0.0]
 *   Per-doc probability [0,1] of adding a ~25 KB field, causing accumulated
 *   bucket size to exceed gTimeseriesBucketMaxSize (kSize rollover).
 *   NOTE: kCachePressure rollover depends on server-side memory state and cannot
 *   be injected at the document level; it is not represented here.
 * @param {number} [options.bucketSpanSeconds=3600]
 *   Bucket time span used when computing time-forward jump sizes. Should match
 *   the collection's bucketMaxSpanSeconds (default granularity = hours = 3600 s).
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

    const rolloverConditions = options.rolloverConditions ?? {};
    const timeForwardChance = rolloverConditions.timeForwardChance ?? 0.0;
    const timeBackwardChance = rolloverConditions.timeBackwardChance ?? 0.0;
    const countRollover = rolloverConditions.countRollover ?? false;
    const sizeRolloverChance = rolloverConditions.sizeRolloverChance ?? 0.0;
    const bucketSpanSeconds = options.bucketSpanSeconds ?? 3600;

    for (const [key, val] of [
        ["timeForwardChance", timeForwardChance],
        ["timeBackwardChance", timeBackwardChance],
        ["sizeRolloverChance", sizeRolloverChance],
    ]) {
        if (typeof val !== "number" || val < 0 || val > 1) {
            throw new Error(`makeMeasurementDocStreamArb: rolloverConditions.${key} must be a number in [0, 1]`);
        }
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
    const extendControlFrequency = options.extendControlFrequency ?? 0.25;
    const newFieldFrequency = options.newFieldFrequency ?? 0.05;
    const explicitArbitraries = options.explicitArbitraries ?? {};

    const defaultDateMin = new Date("1970-01-01T00:00:00.000Z");
    const defaultDateMax = new Date("2038-01-19T03:14:07.000Z");

    const dateMin = dateRange?.min ?? defaultDateMin;
    const dateMax = dateRange?.max ?? defaultDateMax;

    // Exclude the size-rollover field name so generated names never collide with it.
    const baseFieldNameArb = fieldNameArb.filter(
        (name) =>
            ![
                "_id",
                timeFieldname,
                metaFieldname,
                kSizeRolloverFieldName,
                ...Object.keys(explicitArbitraries),
            ].includes(name),
    );

    const fieldNamesArb = fc.array(baseFieldNameArb, {
        minLength: minFields,
        maxLength: maxFields,
    });

    // countRollover: ensure the batch is large enough to trigger a kCount rollover.
    const effectiveMinDocs = countRollover ? Math.max(minDocs, kBucketMaxCount + 1) : minDocs;
    const effectiveMaxDocs = countRollover ? Math.max(maxDocs, kBucketMaxCount + 10) : maxDocs;

    const docCountArb = fc.integer({min: effectiveMinDocs, max: effectiveMaxDocs});

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

        // Time stream: use forward-jump injection when timeForwardChance > 0,
        // otherwise fall back to the standard sensor-like stream.
        const timeStreamArb =
            timeForwardChance > 0
                ? makeTimeForwardStreamArb(
                      docCount,
                      dateMin,
                      dateMax,
                      bucketSpanSeconds,
                      timeBucketing,
                      timeForwardChance,
                  )
                : makeSensorDateMetricStreamArb(
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
            streamFactories: undefined,
            extendControlFrequency: extendControlFrequency,
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

        // Per-doc decisions: each doc has a newFieldFrequency chance of introducing a new field
        // to the schema for all subsequent docs (including itself).
        const newFieldDecisionsArb = fc.array(fc.double({min: 0, max: 1, noNaN: true}), {
            minLength: docCount,
            maxLength: docCount,
        });

        return newFieldDecisionsArb.chain((decisions) => {
            const insertionIndices = decisions.reduce((acc, d, i) => {
                if (d < newFieldFrequency) acc.push(i);
                return acc;
            }, []);

            const newFieldNameArb = baseFieldNameArb.filter((name) => !fieldNames.includes(name));
            const newFieldEventArbs = insertionIndices.map((insertionIdx) =>
                fc
                    .tuple(
                        newFieldNameArb,
                        makeMetricStreamArb(docCount, docCount, extraFieldRanges, undefined, extendControlFrequency),
                    )
                    .map(([fieldName, stream]) => ({fieldName, insertionIdx, stream})),
            );
            const newFieldEventsArb = newFieldEventArbs.length > 0 ? fc.tuple(...newFieldEventArbs) : fc.constant([]);

            return newFieldEventsArb.chain((newFieldEvents) =>
                fc
                    .tuple(timeStreamArb, metaStreamArb, ...Object.values(extraFieldStreamArbs))
                    .map(([timeStream, metaStream, ...extraFieldStream]) => {
                        const docs = [];

                        for (let i = 0; i < docCount; ++i) {
                            const doc = {[timeFieldname]: timeStream[i], [metaFieldname]: metaStream[i]};

                            Object.keys(extraFieldStreamArbs).forEach((name, idx) => {
                                doc[name] = extraFieldStream[idx][i];
                            });

                            for (const {fieldName, insertionIdx, stream} of newFieldEvents) {
                                if (i >= insertionIdx) {
                                    doc[fieldName] = stream[i];
                                }
                            }

                            docs.push(doc);
                        }

                        return docs;
                    })
                    // Apply time-backward and size-rollover injections as a chain so that
                    // the random decisions participate in fast-check's shrinking.
                    .chain((docs) =>
                        applyRolloverInjections(docs, timeFieldname, timeBackwardChance, sizeRolloverChance, dateMin),
                    ),
            );
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
