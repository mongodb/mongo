/*
 * Arbitraries for generating timeseries metrics.
 */

import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

export const allBsonMetricTypes = Object.freeze([
    // basic
    "double",
    "int",
    "long",
    "decimal",
    "string",
    // misc scalar
    "bool",
    "date",
    "timestamp",
    "null",
    "undefined",
    "minKey",
    "maxKey",
    // identifiers / binary
    "objectId",
    "uuid",
    "binData",
    // code
    "javascript",
    "javascriptWithScope",
    "dbPointer",
]);

function _defaultDateRange(dateRange) {
    const defaultDateMin = new Date("1970-01-01T00:00:00.000Z");
    const defaultDateMax = new Date("2038-01-19T03:14:07.000Z");
    return {
        min: dateRange?.min ?? defaultDateMin,
        max: dateRange?.max ?? defaultDateMax,
    };
}

function _defaultIntRange(intRange) {
    return {
        min: intRange?.min ?? -1000,
        max: intRange?.max ?? 1000,
    };
}

function _defaultDoubleRange(doubleRange) {
    return {
        min: doubleRange?.min ?? -1e6,
        max: doubleRange?.max ?? 1e6,
    };
}

function _defaultLongRange(longRange) {
    // Keep "safe-ish" to avoid precision surprises when using JS numbers, then wrap to NumberLong.
    return {
        min: longRange?.min ?? -9_000_000_000_000,
        max: longRange?.max ?? 9_000_000_000_000,
    };
}

function _defaultDecimalRange(decimalRange) {
    return {
        min: decimalRange?.min ?? -1e6,
        max: decimalRange?.max ?? 1e6,
        // number of decimal places in string representation
        scale: decimalRange?.scale ?? 6,
    };
}
/**
 * Make a "single metric" arbitrary. Currently supports most BSON primitive types,
 * still does not support arrays, sub-objects, or regex.
 *
 * Type selection:
 *   - `types` is an array of strings: any subset of allBsonMetricTypes.
 *   - If `types` is omitted or empty, we default to allBsonMetricTypes.
 *
 * Ranges:
 * - ranges.intRange -> {min, max} for int
 * - ranges.doubleRange -> {min, max} for double
 * - ranges.longRange -> {min, max} for long (wrapped to NumberLong)
 * - ranges.decimalRange -> {min, max, scale} for decimal string (wrapped to NumberDecimal)
 * - ranges.dateRange -> {min, max} for date
 *
 * @param {string[]} [types]
 * @param {Object} [ranges]
 * @param {Range} [ranges.intRange]
 * @param {Range} [ranges.doubleRange]
 * @param {Range} [ranges.longRange]
 * @param {{min:number,max:number,scale?:number}} [ranges.decimalRange]
 * @param {{min:Date,max:Date}} [ranges.dateRange]
 *
 * @returns {fc.Arbitrary<any>}
 */
export function makeMetricArb(types, ranges = {}) {
    const typeList = Array.isArray(types) && types.length > 0 ? types : allBsonMetricTypes;

    const {min: intMin, max: intMax} = _defaultIntRange(ranges.intRange);
    const {min: dblMin, max: dblMax} = _defaultDoubleRange(ranges.doubleRange);
    const {min: longMin, max: longMax} = _defaultLongRange(ranges.longRange);
    const {min: decMin, max: decMax, scale: decScale} = _defaultDecimalRange(ranges.decimalRange);
    const {min: dateMin, max: dateMax} = _defaultDateRange(ranges.dateRange);

    const bsonPrimitiveMetricTypes = [];

    if (typeList.includes("int")) {
        bsonPrimitiveMetricTypes.push(fc.integer({min: intMin, max: intMax}));
    }
    if (typeList.includes("double")) {
        bsonPrimitiveMetricTypes.push(fc.double({min: dblMin, max: dblMax, noNaN: true}));
    }
    if (typeList.includes("long")) {
        bsonPrimitiveMetricTypes.push(fc.integer({min: longMin, max: longMax}).map((n) => NumberLong(n)));
    }
    if (typeList.includes("decimal")) {
        bsonPrimitiveMetricTypes.push(
            fc.double({min: decMin, max: decMax, noNaN: true}).map((n) => NumberDecimal(Number(n).toFixed(decScale))),
        );
    }
    if (typeList.includes("string")) {
        bsonPrimitiveMetricTypes.push(fc.string({minLength: 0, maxLength: 32}));
    }
    if (typeList.includes("bool")) {
        bsonPrimitiveMetricTypes.push(fc.boolean());
    }
    if (typeList.includes("date")) {
        bsonPrimitiveMetricTypes.push(fc.date({min: dateMin, max: dateMax}));
    }
    if (typeList.includes("timestamp")) {
        // Timestamp(seconds, increment)
        bsonPrimitiveMetricTypes.push(
            fc
                .tuple(fc.integer({min: 0, max: 0x7fffffff}), fc.integer({min: 0, max: 0x7fffffff}))
                .map(([t, i]) => Timestamp(t, i)),
        );
    }
    if (typeList.includes("null")) {
        bsonPrimitiveMetricTypes.push(fc.constant(null));
    }
    if (typeList.includes("undefined")) {
        bsonPrimitiveMetricTypes.push(fc.constant(undefined));
    }
    if (typeList.includes("minKey")) {
        bsonPrimitiveMetricTypes.push(fc.constant(MinKey()));
    }
    if (typeList.includes("maxKey")) {
        bsonPrimitiveMetricTypes.push(fc.constant(MaxKey()));
    }
    if (typeList.includes("uuid")) {
        bsonPrimitiveMetricTypes.push(fc.uuid());
    }
    if (typeList.includes("objectId")) {
        bsonPrimitiveMetricTypes.push(fc.hexaString({minLength: 24, maxLength: 24}).map(ObjectId));
    }
    if (typeList.includes("binData")) {
        // Keep sizes small and hex-based.
        bsonPrimitiveMetricTypes.push(
            fc
                .tuple(fc.integer({min: 0, max: 255}), fc.hexaString({minLength: 24, maxLength: 24}))
                .map(([subtype, hex]) => HexData(subtype, hex)),
        );
    }
    if (typeList.includes("javascript")) {
        bsonPrimitiveMetricTypes.push(
            fc.string({minLength: 0, maxLength: 64}).map((s) => Code(`function(){ return ${JSON.stringify(s)}; }`)),
        );
    }
    if (typeList.includes("javascriptWithScope")) {
        // Keep scope shallow and JSONy (no nested objects).
        const scopeArb = fc.dictionary(
            fc.string({minLength: 1, maxLength: 8}).filter((n) => n !== "_id"),
            fc.oneof(fc.integer({min: intMin, max: intMax}), fc.string({minLength: 0, maxLength: 16}), fc.boolean()),
            {maxKeys: 4},
        );
        bsonPrimitiveMetricTypes.push(
            fc
                .tuple(fc.string({minLength: 0, maxLength: 64}), scopeArb)
                .map(([s, scope]) => Code(`function(){ return ${JSON.stringify(s)}; }`, scope)),
        );
    }
    if (typeList.includes("dbPointer")) {
        bsonPrimitiveMetricTypes.push(
            fc
                .tuple(
                    fc.string({minLength: 1, maxLength: 16}),
                    fc.hexaString({minLength: 24, maxLength: 24}).map(ObjectId),
                )
                .map(([ns, oid]) => DBPointer(ns, oid)),
        );
    }

    if (bsonPrimitiveMetricTypes.length === 0) {
        throw new Error("makeMetricArb: no metric types enabled");
    }

    return fc.oneof(...bsonPrimitiveMetricTypes);
}

/*
 * Helpers to build streams (arrays) of metrics of a given type, followed by
 * aggregate metric stream arbs that compose these into varied generation.
 *
 * Here is where we will most immediately need to extend with generation
 * patterns meaningful for testing. Of most immediate need are the generation
 * of unique id values, and of date streams that increase in manners that
 * create realistic bucket boundaries.
 */

/**
 * Stream implementation: array of Date metrics.
 *
 * The ranges object is only used to carry ranges for the "date" type and is
 * passed directly into makeMetricArb(['date'], ranges).
 *
 * @param {number} [minLength=0]
 * @param {number} [maxLength=20]
 * @param {Object} [ranges]
 * @param {Range} [ranges.dateRange]
 *
 * @returns {fc.Arbitrary<Date[]>}
 */
export function makeDateMetricStreamArb(minLength = 0, maxLength = 20, ranges = {}) {
    const metricArb = makeMetricArb(["date"], ranges);
    return fc.array(metricArb, {minLength, maxLength});
}

/**
 * Helper to generate step sizes for the sensor-like date stream generator.
 * These are designed to create a variety of realistic cadences and gaps, that will
 * result in useful bucketing behavior.
 */
function generateStepArb(bucketing) {
    const unit = typeof bucketing === "string" ? bucketing : bucketing.unit;
    const gapFactorMin =
        typeof bucketing === "object" && bucketing.gapFactorMin !== undefined ? bucketing.gapFactorMin : 1.0;
    const gapFactorMax =
        typeof bucketing === "object" && bucketing.gapFactorMax !== undefined ? bucketing.gapFactorMax : 2.0;

    function unitToMs(u) {
        switch (u) {
            case "seconds":
                return 1000;
            case "minutes":
                return 60_000;
            case "hours":
                return 3_600_000;
            case "days":
                return 86_400_000;
            default:
                throw new Error(`makeSensorDateMetricStreamArb: unknown bucketing unit: ${u}`);
        }
    }

    const bucketMs = unitToMs(unit);

    // Typical sensor cadence: sub-second to seconds, sometimes up to a couple minutes.
    const smallStep = fc.integer({min: 0, max: 10_000}); // 0s .. 10s
    const mediumStep = fc.integer({min: 10_000, max: 120_000}); // 10s .. 2m

    // Rare gaps that (likely) cross bucket boundaries.
    const gapMinMs = Math.max(1, Math.floor(bucketMs * gapFactorMin));
    const gapMaxMs = Math.max(gapMinMs, Math.floor(bucketMs * gapFactorMax));
    const largeGap = fc.integer({min: gapMinMs, max: gapMaxMs});

    // Weighting: 3:2:1 of many small, some medium, rare large.
    return fc.oneof(smallStep, smallStep, smallStep, mediumStep, mediumStep, largeGap);
}

/* Helper to build a sensible stream of dates given a step arbitrary. */
function generateDateStream(stepArb, dateMin, dateMax, len) {
    return fc.array(stepArb, {minLength: len - 1, maxLength: len - 1}).chain((steps) => {
        const span = dateMax.getTime() - dateMin.getTime();

        let total = 0;
        for (const s of steps) total += s;

        // If the generated total span doesn't fit, compress proportionally (preserving monotonicity).
        let scaledSteps = steps;
        if (total > span && span > 0) {
            const scale = span / total;
            scaledSteps = steps.map((s) => Math.max(1, Math.floor(s * scale)));
            total = 0;
            for (const s of scaledSteps) total += s;
        }

        const startMaxMs = Math.max(dateMin.getTime(), dateMax.getTime() - total);
        const startArb = fc.integer({min: dateMin.getTime(), max: startMaxMs}).map((ms) => new Date(ms));

        return startArb.map((start) => {
            const out = [start];
            let cur = start.getTime();
            for (const s of scaledSteps) {
                cur += s;
                out.push(new Date(cur));
            }
            return out;
        });
    });
}

/**
 * Stream implementation: array of Date metrics that tend to increase like a sensor.
 *
 * @param {('seconds'|'minutes'|'hours'|'days'|{unit:'seconds'|'minutes'|'hours'|'days', gapFactorMin?:number, gapFactorMax?:number})} [bucketing='hours']
 * @returns {fc.Arbitrary<Date[]>}
 */
export function makeSensorDateMetricStreamArb(minLength = 0, maxLength = 20, ranges = {}, bucketing = "hours") {
    const {min: dateMin, max: dateMax} = _defaultDateRange(ranges.dateRange);

    const lenArb = fc.integer({min: minLength, max: maxLength});
    const stepArb = generateStepArb(bucketing);

    return lenArb.chain((len) => {
        if (len <= 0) {
            return fc.constant([]);
        }
        if (len === 1) {
            return fc.date({min: dateMin, max: dateMax}).map((d) => [d]);
        }

        return generateDateStream(stepArb, dateMin, dateMax, len);
    });
}

/**
 * Generic stream implementation for ANY metric type supported by makeMetricArb.
 *
 * @param {string} type - one of allBsonMetricTypes
 * @returns {fc.Arbitrary<any[]>}
 */
export function makeMetricTypeStreamArb(type, minLength = 0, maxLength = 20, ranges = {}) {
    // Special-case: date streams can use the sensor-like generator if the caller wants it.
    if (type === "date" && ranges?.useSensorDateStream) {
        return makeSensorDateMetricStreamArb(minLength, maxLength, ranges, ranges?.timeBucketing ?? "hours");
    }
    const metricArb = makeMetricArb([type], ranges);
    return fc.array(metricArb, {minLength, maxLength});
}

/**
 * Parent "metric stream" arbitrary.
 *
 * By default, chooses between streams for ALL supported metric types
 * (excluding object/array), with "date" using the sensor-like stream.
 *
 * Callers can override which implementations to oneof between via `streamFactories`.
 */
export function makeMetricStreamArb(minLength = 0, maxLength = 20, ranges = {}, streamFactories) {
    if (Array.isArray(streamFactories) && streamFactories.length > 0) {
        const streamArbs = streamFactories.map((fn) => fn(minLength, maxLength, ranges));
        return fc.oneof(...streamArbs);
    }

    // Default: build a stream arb per type.
    const arbs = [];
    for (const t of allBsonMetricTypes) {
        arbs.push(makeMetricTypeStreamArb(t, minLength, maxLength, ranges));
    }
    return fc.oneof(...arbs);
}
/**
 * Simple defaults if you just want "some" ts-style docs.
 * (metaValue omitted -> meta chosen from metric arb per doc/stream)
 */

export const simpleMetricArb = makeMetricArb(); // all types, default ranges

export const simpleMetricStreamArb = makeMetricStreamArb(); // all types, default ranges
