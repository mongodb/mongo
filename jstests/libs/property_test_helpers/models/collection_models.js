/*
 * Fast-check models for collections.
 * See property_test_helpers/README.md for more detail on the design.
 */
import {
    defaultDocModel,
    timeseriesDocModel
} from "jstests/libs/property_test_helpers/models/document_models.js";
import {
    defaultIndexModel,
    timeseriesIndexModel
} from "jstests/libs/property_test_helpers/models/index_models.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

// Maximum number of documents that our collection model can generate.
const kMaxNumQueries = 250;

// An array of [0...249] to label our documents with.
const docIds = [];
for (let i = 0; i < kMaxNumQueries; i++) {
    docIds.push(i);
}
const uniqueIdsArb =
    fc.shuffledSubarray(docIds, {minLength: kMaxNumQueries, maxLength: kMaxNumQueries});

function getDocsModel(isTS) {
    const docModel = isTS ? timeseriesDocModel : defaultDocModel;
    // The size=+2 argument tells fc.array to generate array sizes closer to the max than the min.
    // This way the average number of documents produced is >100, which means our queries will be
    // less likely to produce empty results. The size argument does not affect minimization. On
    // failure, fast-check will still minimize down to 1 document if possible.
    // These docs are 'unlabeled' because we have not assigned them unique _ids yet.
    const unlabeledDocsModel =
        fc.array(docModel, {minLength: 1, maxLength: kMaxNumQueries, size: '+2'});
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

export function getCollectionModel({isTS = false} = {}) {
    const indexModel = isTS ? timeseriesIndexModel : defaultIndexModel;
    const indexesModel = fc.array(indexModel, {minLength: 0, maxLength: 7});

    // TODO SERVER-93783 as part of the collection model, we'll generate different query knobs that
    // can be set on a collection.
    return fc.record({isTS: fc.constant(isTS), docs: getDocsModel(isTS), indexes: indexesModel});
}
