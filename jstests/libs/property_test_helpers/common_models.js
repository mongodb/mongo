import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {getDatasetModel, getDocModel} from "jstests/libs/property_test_helpers/models/document_models.js";
import {getAggPipelineModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

export function createStabilityWorkload(numQueriesPerRun) {
    // TODO SERVER-108077 SERVER-106983 when these tickets are complete, remove filters and allow
    // ORs.
    const aggModel = getAggPipelineModel({allowOrs: false, deterministicBag: false}).filter((q) => {
        const asStr = JSON.stringify(q);
        // The query cannot contain any of these strings, as they are linked to the issues above.
        return ["$not", "$exists", "array"].every((expr) => !asStr.includes(expr));
    });

    return makeWorkloadModel({
        collModel: getCollectionModel({
            docsModel: getDatasetModel(
                // TODO SERVER-100515 reenable unicode.
                {
                    maxNumDocs: 2000,
                    docModel: getDocModel({allowUnicode: false, allowNullBytes: false}),
                },
            ),
        }),
        aggModel,
        numQueriesPerRun,
        // Include one extra param representing the number of buckets in the analyze command.
        // Use 5 as the minimum to avoid an error about the number of buckets needing to be at least
        // the number of types in the dataset.
        extraParamsModel: fc.record({numberBuckets: fc.integer({min: 5, max: 2000})}),
    });
}
