/*
 * Arbitraries for generating timeseries metrics.
 */

import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

/**
 * Make a "single metric" arbitrary. Currently supports ints, dates, and UUIDs, this will need
 * to be extended with additional types. Dates produce non-extended range by default.
 *
 * Type selection:
 *   - `types` is an array of strings: any subset of ["int", "date", "uuid"]
 *   - If `types` is omitted or empty, we default to all three.
 *
 * Ranges:
 *   - ranges.intRange  -> {min, max} for int
 *   - ranges.dateRange -> {min, max} for date
 *
 * @param {('int'|'date'|'uuid')[]} [types]
 * @param {Object} [ranges]
 * @param {Range} [ranges.intRange]
 * @param {Range} [ranges.dateRange]
 *
 * @returns {fc.Arbitrary<any>}
 */
export function makeMetricArb(types, ranges) {
    const typeList = Array.isArray(types) && types.length > 0 ? types : ["int", "date", "uuid"];

    const {intRange, dateRange} = ranges || {};

    // Default ranges
    const intMin = intRange?.min ?? -1000;
    const intMax = intRange?.max ?? 1000;

    const defaultDateMin = new Date("1970-01-01T00:00:00.000Z");
    const defaultDateMax = new Date("2038-01-19T03:14:07.000Z");

    const dateMin = dateRange?.min ?? defaultDateMin;
    const dateMax = dateRange?.max ?? defaultDateMax;

    const pieces = [];

    if (typeList.includes("int")) {
        pieces.push(fc.integer({min: intMin, max: intMax}));
    }
    if (typeList.includes("date")) {
        pieces.push(fc.date({min: dateMin, max: dateMax}));
    }
    if (typeList.includes("uuid")) {
        pieces.push(fc.uuid());
    }

    if (pieces.length === 0) {
        throw new Error("makeMetricArb: no metric types enabled");
    }

    return fc.oneof(...pieces);
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
 * Stream implementation: array of int *metrics*.
 *
 * The ranges object is only used to carry ranges for the "int" type and is
 * passed directly into makeMetricArb(['int'], ranges).
 *
 * @param {number} [minLength=0]
 * @param {number} [maxLength=20]
 * @param {Object} [ranges]
 * @param {Range} [ranges.intRange]
 *
 * @returns {fc.Arbitrary<number[]>}
 */
export function makeIntMetricStreamArb(minLength = 0, maxLength = 20, ranges = {}) {
    const metricArb = makeMetricArb(["int"], ranges);
    return fc.array(metricArb, {minLength, maxLength});
}

/**
 * Stream implementation: array of UUID *metrics* (strings).
 *
 * ranges are not used, but are passed for conformity with other stream factories.
 *
 * @param {number} [minLength=0]
 * @param {number} [maxLength=20]
 * @param {Object} [ranges]
 *
 * @returns {fc.Arbitrary<string[]>}
 */
export function makeUuidMetricStreamArb(minLength = 0, maxLength = 20, ranges = {}) {
    const metricArb = makeMetricArb(["uuid"], ranges);
    return fc.array(metricArb, {minLength, maxLength});
}

/**
 * Parent "metric stream" arbitrary.
 *
 * By default, chooses between:
 *   - stream of Date        (makeDateMetricStreamArb)
 *   - stream of int         (makeIntMetricStreamArb)
 *   - stream of uuid string (makeUuidMetricStreamArb)
 *
 * Callers can override which implementations to oneof between via
 * `streamFactories`, which should be an array of functions of the form:
 *
 *   (minLength: number, maxLength: number, ranges: {intRange?, dateRange?}) => Arbitrary<any[]>
 *
 * @param {number} [minLength=0]
 * @param {number} [maxLength=20]
 * @param {Object} [ranges]
 * @param {Range} [ranges.intRange]
 * @param {Range} [ranges.dateRange]
 * @param {Array<Function>} [streamFactories]
 *   Default: [makeDateMetricStreamArb, makeIntMetricStreamArb, makeUuidMetricStreamArb]
 *
 * @returns {fc.Arbitrary<any[]>}
 */
export function makeMetricStreamArb(minLength = 0, maxLength = 20, ranges = {}, streamFactories) {
    const factories =
        Array.isArray(streamFactories) && streamFactories.length > 0
            ? streamFactories
            : [makeDateMetricStreamArb, makeIntMetricStreamArb, makeUuidMetricStreamArb];

    const streamArbs = factories.map((fn) => fn(minLength, maxLength, ranges));

    return fc.oneof(...streamArbs);
}

/**
 * Simple defaults if you just want "some" ts-style docs.
 * (metaValue omitted -> meta chosen from metric arb per doc/stream)
 */

export const simpleMetricArb = makeMetricArb(); // all types, default ranges

export const simpleMetricStreamArb = makeMetricStreamArb(); // all types, default ranges
