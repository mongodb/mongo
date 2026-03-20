/*
 * Arbitraries for generating timeseries metrics.
 */

import {fc} from "jstests/third_party/fast_check/fc-4.6.0.js";

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

/** Shim for hexa migration, see https://fast-check.dev/docs/migration-guide/from-3.x-to-4.x/#hexa-or-hexastring
 *
 * @returns fc.Arbitrary<string>
 */
function hexa() {
    const items = "0123456789abcdef";
    return fc.integer({min: 0, max: 15}).map(
        (n) => items[n],
        (c) => items.indexOf(c),
    );
}

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
        // In regular collections, the special value of Timestamp(0, 0) will generate a server-side timestamp.
        // This value needs to be avoided to keep the two collection types in sync, and is a known difference
        // between regular and Timeseries colleciton behavior.
        bsonPrimitiveMetricTypes.push(
            fc
                .tuple(fc.integer({min: 0, max: 0x7fffffff}), fc.integer({min: 1, max: 0x7fffffff}))
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
        bsonPrimitiveMetricTypes.push(fc.string({minLength: 24, maxLength: 24, unit: hexa()}).map(ObjectId));
    }
    if (typeList.includes("binData")) {
        // Keep sizes small and hex-based.
        bsonPrimitiveMetricTypes.push(
            fc
                .tuple(fc.integer({min: 0, max: 255}), fc.string({minLength: 24, maxLength: 24, unit: hexa()}))
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
                    fc.string({minLength: 24, maxLength: 24, unit: hexa()}).map(ObjectId),
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
 * Metric stream arbitrary that produces run* of values, where each run is either:
 *  - a repetition of the same value, or
 *  - a monotonic sequence (increasing or decreasing)
 *
 * @param {fc.Arbitrary<any>} metricArb - generates a single metric value
 * @param {Object} [opts]
 * @param {number} [opts.minLength=0]
 * @param {number} [opts.maxLength=20]
 * @param {number} [opts.maxRuns=6]
 * @param {number} [opts.maxRunLength=8]
 * @param {number} [opts.stepMax=10] - max absolute step for numeric/date/string monotone runs
 * @param {number} [opts.maxMonoStringBytes=16] - max ASCII byte length treated as unsigned 16-byte integer
 * @returns {fc.Arbitrary<any[]>}
 */
export function makeRunnyMetricStreamArb(metricArb, opts = {}) {
    const {minLength = 0, maxLength = 20, maxRuns = 6, maxRunLength = 8, stepMax = 10, maxMonoStringBytes = 16} = opts;

    assert(minLength >= 0, "minLength must be non-negative");

    function isAsciiString(s) {
        for (let i = 0; i < s.length; i++) {
            if (s.charCodeAt(i) > 0x7f) return false;
        }
        return true;
    }

    /**
     * Represent strings of 16 or less bytes  as an unsigned width-byte integer, big-endian, with s
     * occupying the least-significant bytes (left pad with 0x00). This is intended to mirror the
     * bsoncolumn encoding of short strings.
     *
     * @param {string} s
     * @param {number} width
     * @returns {bigint}
     */
    function string16ToBigInt(s, width) {
        const bytes = new Array(width).fill(0);
        const start = width - s.length;
        for (let i = 0; i < s.length; i++) {
            bytes[start + i] = s.charCodeAt(i) & 0xff;
        }

        let x = 0n;
        for (const b of bytes) {
            x = (x << 8n) | BigInt(b);
        }
        return x;
    }

    /**
     * Inverse of string16ToBigInt, clamping into the width-byte space as needed.
     *
     * @param {bigint} x
     * @param {number} width
     * @returns {string}
     */
    function bigIntToString16(x, width) {
        // Clamp into [0, 2^(8*width)-1] to keep fixed width.
        const mod = 1n << BigInt(8 * width);
        let y = x % mod;
        if (y < 0) y += mod;

        // Extract big-endian bytes.
        const bytes = new Array(width);
        for (let i = width - 1; i >= 0; i--) {
            bytes[i] = Number(y & 0xffn);
            y >>= 8n;
        }

        // Strip leading 0x00 padding.
        let start = 0;
        while (start < width && bytes[start] === 0) start++;

        const slice = bytes.slice(start);
        return String.fromCharCode(...slice);
    }

    /**
     * Determine the "domain" of a metric value for the purposes of run generation. This determines how we
     * convert to/from number for monotone step generation, and also determines when we degrade to repeat runs
     *
     * @param {object} v
     * @returns {string} - "date", "number", "long", "decimal", "string16", or "other"
     */
    function domainOf(v) {
        if (v instanceof Date) return "date";
        if (typeof v === "number") return "number";
        if (typeof NumberLong === "function" && v instanceof NumberLong) return "long";
        if (typeof NumberDecimal === "function" && v instanceof NumberDecimal) return "decimal";
        if (typeof v === "string" && isAsciiString(v) && v.length <= maxMonoStringBytes) return "string16";
        return "other";
    }

    /**
     * Convert a metric value to a number for the purposes of monotone step generation.
     * @param {string} domain
     * @returns {number|bigint}
     */
    function toNumber(v, domain) {
        switch (domain) {
            case "number":
                return v;
            case "date":
                return v.getTime();
            case "long":
                return typeof v.toNumber === "function" ? v.toNumber() : Number(v);
            case "decimal":
                return Number(v.toString());
            case "string16":
                return string16ToBigInt(v, maxMonoStringBytes);
            default:
                return NaN;
        }
    }

    /**
     * Convert a number back to a metric value after monotone step generation.
     *
     * @param {number|bigint} x
     * @param {string} domain
     * @returns {object}
     */
    function fromNumber(x, domain) {
        switch (domain) {
            case "number":
                return x;
            case "date":
                return new Date(Math.trunc(x));
            case "long":
                return NumberLong(String(Math.trunc(x)));
            case "decimal":
                return NumberDecimal(String(x));
            case "string16":
                return bigIntToString16(x, maxMonoStringBytes);
            default:
                return x;
        }
    }

    const targetLenArb = fc.integer({min: minLength, max: maxLength});

    const runDescArb = fc.record({
        kind: fc.constantFrom("repeat", "mono"),
        len: fc.integer({min: 1, max: maxRunLength}),
        dir: fc.constantFrom(1, -1),
        step: fc.integer({min: 0, max: stepMax}),
    });

    return targetLenArb.chain((targetLen) => {
        if (targetLen === 0) return fc.constant([]);

        const runsArb = fc.array(runDescArb, {minLength: 1, maxLength: maxRuns});

        return fc.tuple(metricArb, runsArb).map(([seedValue, runs]) => {
            const out = [];
            let cur = seedValue;

            // If the initial value isn't monotone-capable, all mono runs degrade to repeat.
            const seedDomain = domainOf(seedValue);

            for (const r of runs) {
                if (out.length >= targetLen) break;

                const remaining = targetLen - out.length;
                const runLen = Math.min(r.len, remaining);

                if (r.kind === "repeat" || seedDomain === "other") {
                    for (let i = 0; i < runLen; i++) out.push(cur);
                    continue;
                }

                const domain = domainOf(cur);
                if (domain === "other") {
                    for (let i = 0; i < runLen; i++) out.push(cur);
                    continue;
                }

                let x = toNumber(cur, domain);

                for (let i = 0; i < runLen; i++) {
                    out.push(fromNumber(x, domain));
                    if (domain === "string16") {
                        x = x + BigInt(r.dir * r.step);
                    } else {
                        x = x + r.dir * r.step;
                    }
                }

                cur = out[out.length - 1];
            }

            // If we didn't fill completely, pad by repeating the last value.
            while (out.length < targetLen) out.push(cur);

            return out;
        });
    });
}

/**
 * Simple defaults if you just want "some" ts-style docs.
 * (metaValue omitted -> meta chosen from metric arb per doc/stream)
 */

/**
 * Stream generator for a single stream whose values are intentionally mixed-type.
 *
 * It chooses two distinct BSON metric types, generates one stream of each type,
 * and then intermixes the resulting values position-by-position.
 */
export function makeMixedTypeMetricStreamArb(minLength = 0, maxLength = 20, ranges = {}) {
    const distinctTypePairArb = fc
        .tuple(fc.constantFrom(...allBsonMetricTypes), fc.constantFrom(...allBsonMetricTypes))
        .filter(([lhsType, rhsType]) => lhsType !== rhsType);

    return distinctTypePairArb.chain(([lhsType, rhsType]) => {
        const lhsStreamArb = makeMetricTypeStreamArb(lhsType, minLength, maxLength, ranges);
        const rhsStreamArb = makeMetricTypeStreamArb(rhsType, minLength, maxLength, ranges);

        return fc.tuple(lhsStreamArb, rhsStreamArb).chain(([lhsStream, rhsStream]) => {
            const streamLength = lhsStream.length < rhsStream.length ? lhsStream.length : rhsStream.length;

            return fc.array(fc.boolean(), {minLength: streamLength, maxLength: streamLength}).map((chooseLhs) => {
                const mixedStream = [];
                for (let i = 0; i < streamLength; ++i) {
                    mixedStream.push(chooseLhs[i] ? lhsStream[i] : rhsStream[i]);
                }
                return mixedStream;
            });
        });
    });
}

export const simpleMetricArb = makeMetricArb(); // all types, default ranges

export const simpleMetricStreamArb = makeMetricStreamArb(); // all types, default ranges
