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

export function getCollectionModel(
    {isTS = false, allowPartialIndexes = false, indexesModel, docsModel} = {}) {
    // If no documents model or index model is provided, assume the default.
    if (!docsModel) {
        docsModel = getDocsModel({isTS});
    }
    if (!indexesModel) {
        const indexModel = isTS ? getTimeSeriesIndexModel({allowPartialIndexes})
                                : getIndexModel({allowPartialIndexes});
        indexesModel = fc.array(indexModel, {minLength: 0, maxLength: 15, size: '+2'});
    }

    return fc.record({isTS: fc.constant(isTS), docs: docsModel, indexes: indexesModel});
}
