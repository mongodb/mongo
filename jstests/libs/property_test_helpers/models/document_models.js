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
import {
    dateArb,
    fieldArb,
    intArb,
    scalarArb
} from "jstests/libs/property_test_helpers/models/basic_models.js";
import {oneof} from "jstests/libs/property_test_helpers/models/model_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

const mFieldModel = fc.record({m1: scalarArb, m2: scalarArb});
const arrayFieldElementArb = oneof(scalarArb, fc.array(scalarArb, {maxLength: 2}), mFieldModel);
const arrayFieldModel = fc.array(arrayFieldElementArb, {maxLength: 5});

const defaultDocModel = fc.record({
    _id: intArb,
    t: dateArb,
    m: mFieldModel,
    array: oneof(scalarArb, arrayFieldModel),
    a: scalarArb,
    b: scalarArb
});
// `defaultDocModel` and `timeseriesDocModel` may diverge later. By exporting two models, we make it
// clear these models are separate so existing tests don't rely on behavior specific to `docModel`.
const timeseriesDocModel = defaultDocModel;

// Maximum number of documents that our collection model can generate.
const kMaxNumDocs = 250;

// An array of [0...249] to label our documents with.
const docIds = [];
for (let i = 0; i < kMaxNumDocs; i++) {
    docIds.push(i);
}
const uniqueIdsArb = fc.shuffledSubarray(docIds, {minLength: kMaxNumDocs, maxLength: kMaxNumDocs});

export function getDocsModel({isTS = false, docModel} = {}) {
    if (!docModel) {
        docModel = isTS ? timeseriesDocModel : defaultDocModel;
    }

    // The size=+2 argument tells fc.array to generate array sizes closer to the max than the min.
    // This way the average number of documents produced is >100, which means our queries will be
    // less likely to produce empty results. The size argument does not affect minimization. On
    // failure, fast-check will still minimize down to 1 document if possible.
    // These docs are 'unlabeled' because we have not assigned them unique _ids yet.
    const unlabeledDocsModel =
        fc.array(docModel, {minLength: 1, maxLength: kMaxNumDocs, size: '+2'});
    // Now label the docs with unique _ids.
    return fc.record({unlabeledDocs: unlabeledDocsModel, _ids: uniqueIdsArb})
        .map(({unlabeledDocs, _ids}) => {
            // We can run into issues with fast-check if we mutate generated values.
            // We'll make new docs and add _id to it.
            return unlabeledDocs.map((oldDoc, ix) => {
                // Make sure our unique _id overwrites the original doc _id, by
                // putting it last in the list.
                return Object.assign({}, oldDoc, {_id: _ids[ix]});
            });
        });
}

/**
 * Similar to getDocModel(), but generates more deeply nested data, and does not allow arrays.
 *
 * 'keyArb' is the arbitrary used to generate object keys. It's not a strict guarantee that objects
 * produced will have nesting depth at most 'approxMaxDepth' (hence "approx"); see note below.
 */
export function getNestedDocModelNoArray({keyArb, approxMaxDepth, maxObjectKeys} = {}) {
    if (!keyArb) {
        // Re-use the standard field arbitrary if keyArb is not provided. Note that some of these
        // keys could be dotted, so in reality we may end up with an object slightly more nested
        // than 'approxMaxDepth'.
        keyArb = fieldArb;
    }
    if (!maxObjectKeys) {
        maxObjectKeys = 5;
    }

    return fc
        .letrec((tie) => ({
                    // A value in an object can be our leaf arbitrary, or it can be a nested object.
                    value: fc.oneof({maxDepth: approxMaxDepth}, scalarArb, tie("object")),
                    object: fc.dictionary(keyArb, tie("value"), {maxKeys: maxObjectKeys}),
                }))
        .object;
}
