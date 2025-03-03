/*
 * Fast-check document model for our core property tests.
 *
 * The schema is intended to work for time-series and regular collections.
 *
 * As part of the core PBT design, we intentionally support a narrow set of basic MQL functionality,
 * which allows us to make stronger assertions than a robust fuzzer might. For documents, this
 * means:
 *     - We allow null fields but not missing fields to avoid SERVER-12869, where covering plans
 *       cannot distinguish between null and missing.
 *     - We also only allow a minimal set of types in test data. For example functions as values in
 *       documents are not allowed, nor is undefined, NaN, Symbols, etc.
 * Types we allow are integers, booleans, dates, strings, null, and limited depth objects and
 * arrays.
 * See property_test_helpers/README.md for more detail on the design.
 */
import {scalarArb} from "jstests/libs/property_test_helpers/models/basic_models.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

const mFieldModel = fc.record({m1: scalarArb, m2: scalarArb});
const arrayFieldElementArb = fc.oneof(scalarArb, fc.array(scalarArb, {maxLength: 2}), mFieldModel);
const arrayFieldModel = fc.array(arrayFieldElementArb, {maxLength: 5});

export const defaultDocModel = fc.record({
    _id: fc.integer().map(i => NumberInt(i)),
    t: fc.date({min: new Date(-100), max: new Date(100)}),
    m: mFieldModel,
    array: fc.oneof(scalarArb, arrayFieldModel),
    a: scalarArb,
    b: scalarArb
});
// `defaultDocModel` and `timeseriesDocModel` may diverge later. By exporting two models, we make it
// clear these models are separate so existing tests don't rely on behavior specific to `docModel`.
export const timeseriesDocModel = defaultDocModel;