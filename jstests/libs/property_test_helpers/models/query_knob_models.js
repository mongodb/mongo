/*
 * Fast-check models for query knob settings.
 */
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

const kInt32Min = -2147483648;
const kInt32Max = 2147483647;

const clampCommon = (min, max) => (u) => Math.max(min, Math.min(max, Math.trunc(u)));
const clampInt32 = clampCommon(kInt32Min, kInt32Max);
const clampInt64 = clampCommon(Number.MIN_SAFE_INTEGER, Number.MAX_SAFE_INTEGER);

function deriveConstraintsFromBounds(knobConstraints) {
    let min, minInclusive, max, maxInclusive;
    for (const {kind, value} of knobConstraints) {
        switch (kind) {
            case "gte":
                if (min === undefined || value > min) {
                    min = value;
                    minInclusive = true;
                }
                break;
            case "gt":
                if (min === undefined || value > min || (value === min && minInclusive)) {
                    min = value;
                    minInclusive = false;
                }
                break;
            case "lte":
                if (max === undefined || value < max) {
                    max = value;
                    maxInclusive = true;
                }
                break;
            case "lt":
                if (max === undefined || value < max || (value === max && maxInclusive)) {
                    max = value;
                    maxInclusive = false;
                }
                break;
        }
    }
    const bounds = {};
    if (min !== undefined) {
        bounds.min = min;
        bounds.minInclusive = minInclusive;
    }
    if (max !== undefined) {
        bounds.max = max;
        bounds.maxInclusive = maxInclusive;
    }
    return bounds;
}

function makeNumericKnob(knob, constraint, alpha = 2) {
    let {min, minInclusive = true, max, maxInclusive = true} = constraint;
    if (!minInclusive && (knob.type === "int" || knob.type === "long long")) {
        min = min + 1;
        minInclusive = true;
    }
    if (!maxInclusive && (knob.type === "int" || knob.type === "long long")) {
        max = max - 1;
        maxInclusive = true;
    }
    let arb =
        min !== undefined
            ? // Generate a pareto distribution shifted to start from the given lower bound.
              fc
                  .double({min: Number.EPSILON, max: 1.0, noNaN: true})
                  .map((u) => min + (Math.abs(min) + 1) * (Math.pow(u, -1.0 / alpha) - 1))
            : // Fallback to a normal distribution if no lower bound is provided.
              fc.double({noNaN: true, ...constraint});
    // Clip to the upper bound if provided.
    if (max !== undefined) {
        arb = arb.map((u) => Math.min(u, max));
    }
    // Truncate to integers and clamp to the type's representable range.
    switch (knob.type) {
        case "int":
            arb = arb.map(clampInt32);
            break;
        case "long long":
            arb = arb.map(clampInt64);
            break;
    }

    // The integer +1/-1 adjustment above does not apply to doubles, so the generator can still emit
    // an exclusive bound exactly (e.g. gt: 1.0 -> 1.0 when the pareto factor is 1). Filter those out
    // so we never produce a value the server's validator rejects.
    if (min !== undefined && !minInclusive) {
        arb = arb.filter((value) => value > min);
    }
    if (max !== undefined && !maxInclusive) {
        arb = arb.filter((value) => value < max);
    }

    // Filter out the default value. Compare numerically since long long defaults are reported as
    // NumberLong objects, which never compare strictly equal to plain JS numbers.
    assert(knob.default !== undefined);
    return arb.filter((value) => value !== Number(knob.default));
}

function makeEnumKnob(knob, constraint) {
    // Filter the default out of the allowed values.
    let values = constraint.allowedValues ?? knob.allowedValues;
    assert(values, "enum knob must have allowed values", {knob});
    assert(knob.default !== undefined);
    values = values.filter((v) => v !== knob.default);

    // Do not emit the knob if there are no non-default values.
    if (values.length === 0) {
        return fc.constant(null);
    }
    // Pick enum values with uniform probability.
    return fc.constantFrom(...values);
}

function makeBooleanKnob(knob) {
    // The only non-default bool value is the negation of the default value.
    assert(knob.default !== undefined);
    return fc.constant(!knob.default);
}

function removeNullValues(obj) {
    let buffer = {};
    for (const [name, value] of Object.entries(obj)) {
        if (value !== null) {
            buffer[name] = value;
        }
    }
    return buffer;
}

/**
 * Builds a fast-check model from the result of `db.getSiblingDB("admin").aggregate([{$listQueryKnobs: {}}]).toArray()`.
 * Each knob is optional so fast-check can shrink to the minimal reproducing set.
 * Optional `constraints` map from knob name to bounds passed directly to the fc arbitrary (e.g.
 * `{min, max}` for integer/double knobs).
 */
export function buildQueryKnobsModel(schema, probability = 0.75, alpha = 2) {
    // Make the knobs have a 'defaultProbability' chance of being null.
    const freq = Math.trunc(1.0 / (1.0 - probability));
    const maybeDefault = (arb) => fc.option(arb, {freq});
    const knobArbs = {};
    for (const knob of schema) {
        const constraint = deriveConstraintsFromBounds(knob.bounds ?? []);
        let arb;
        switch (knob.type) {
            case "bool":
                arb = makeBooleanKnob(knob);
                break;
            case "enum":
                arb = makeEnumKnob(knob, constraint);
                break;
            case "double":
            case "int":
            case "long long":
                arb = makeNumericKnob(knob, constraint, alpha);
                break;
            default:
                assert(false, "unexpected knob type", {knob});
        }
        knobArbs[knob.name] = maybeDefault(arb);
    }
    return fc.record(knobArbs).map(removeNullValues);
}
