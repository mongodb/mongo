/*
 * Fast-check models for collections.
 * See property_test_helpers/README.md for more detail on the design.
 */
import {getDatasetModel} from "jstests/libs/property_test_helpers/models/document_models.js";
import {getIndexesModel} from "jstests/libs/property_test_helpers/models/index_models.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

export function getCollectionModel({isTS = false, indexesModel, docsModel} = {}) {
    const isTSCollection = TestData.isTimeseriesTestSuite || isTS;
    // If no documents model or index model is provided, assume the default.
    if (!docsModel) {
        docsModel = getDatasetModel();
    }
    if (!indexesModel) {
        indexesModel = getIndexesModel({isTS: isTSCollection});
    }

    // TODO SERVER-121084: Reevaluate whether metaField='' should be allowed.
    const metaFieldArb = isTSCollection ? fc.constant("m") : fc.constant(undefined);
    return fc.record({
        isTS: fc.constant(isTSCollection),
        metaField: metaFieldArb,
        docs: docsModel,
        indexes: indexesModel,
    });
}
