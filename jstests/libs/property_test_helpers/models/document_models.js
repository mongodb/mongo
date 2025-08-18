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
    getScalarArb,
    intArb
} from "jstests/libs/property_test_helpers/models/basic_models.js";
import {oneof} from "jstests/libs/property_test_helpers/models/model_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

export function getDocModel({allowUnicode, allowNullBytes} = {}) {
    const scalarArb = getScalarArb({allowUnicode, allowNullBytes});

    const mFieldModel = fc.record({m1: scalarArb, m2: scalarArb});
    const arrayFieldElementArb = oneof(scalarArb, fc.array(scalarArb, {maxLength: 2}), mFieldModel);
    const arrayFieldModel = fc.array(arrayFieldElementArb, {maxLength: 5});

    return fc.record({
        _id: intArb,
        t: dateArb,
        m: mFieldModel,
        array: oneof(scalarArb, arrayFieldModel),
        a: scalarArb,
        b: scalarArb
    });
}

export function getDatasetModel({maxNumDocs = 250, docModel} = {}) {
    if (!docModel) {
        // If no document model is provided, assume the default.
        docModel = getDocModel();
    }

    // An array of [0...maxNumDocs] to label our documents with.
    const docIds = [];
    for (let i = 0; i < maxNumDocs; i++) {
        docIds.push(i);
    }
    const uniqueIdsArb =
        fc.shuffledSubarray(docIds, {minLength: maxNumDocs, maxLength: maxNumDocs});

    // The size=+2 argument tells fc.array to generate array sizes closer to the max than the min.
    // This way the average number of documents produced is >100, which means our queries will be
    // less likely to produce empty results. The size argument does not affect minimization. On
    // failure, fast-check will still minimize down to 1 document if possible.
    // These docs are 'unlabeled' because we have not assigned them unique _ids yet.
    const unlabeledDocsModel =
        fc.array(docModel, {minLength: 1, maxLength: maxNumDocs, size: '+2'});
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
