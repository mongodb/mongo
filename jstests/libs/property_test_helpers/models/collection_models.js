/*
 * Fast-check models for collections.
 * See property_test_helpers/README.md for more detail on the design.
 */
import {getDocsModel} from "jstests/libs/property_test_helpers/models/document_models.js";
import {
    getIndexModel,
    getTimeSeriesIndexModel
} from "jstests/libs/property_test_helpers/models/index_models.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

export function getCollectionModel({isTS = false, allowPartialIndexes = false} = {}) {
    const indexModel = isTS ? getTimeSeriesIndexModel({allowPartialIndexes})
                            : getIndexModel({allowPartialIndexes});
    const indexesModel = fc.array(indexModel, {minLength: 0, maxLength: 15, size: '+2'});

    // TODO SERVER-93783 as part of the collection model, we'll generate different query knobs that
    // can be set on a collection.
    return fc.record({isTS: fc.constant(isTS), docs: getDocsModel(isTS), indexes: indexesModel});
}
