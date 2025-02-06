/* Check the cardinality estimation of very simple predicates using histograms by running
   the predicate itself and comparing the number of documents matched to the estimate.

   In this test, we use distributions that allow for "perfect" histograms, that is,
   histograms where, even with the information loss, perfect estimates can be made.

   Simularily, the predicates used are those that can be estimated perfectly
   (except for the occasional off-by-one errors)
*/

import {
    getAllPlans,
} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {ArrayDataset} from 'jstests/noPassthroughWithMongod/query/cbr/lib/datasets/array.js';
import {BooleanDataset} from 'jstests/noPassthroughWithMongod/query/cbr/lib/datasets/boolean.js';
import {
    DateDataset,
    TimestampDataset
} from 'jstests/noPassthroughWithMongod/query/cbr/lib/datasets/date_time.js';
import {
    OneHoleDataset,
    OnePeakDataset,
    SkewedDataset,
    ThreePeakDataset,
    UniformDataset
} from "jstests/noPassthroughWithMongod/query/cbr/lib/datasets/distributions.js";
import {
    MixedNumbersDataset,
    MixedTypesDataset
} from "jstests/noPassthroughWithMongod/query/cbr/lib/datasets/mixed_types.js";
import {
    TwoFieldDataset
} from "jstests/noPassthroughWithMongod/query/cbr/lib/datasets/multifield.js";
import {NumberDataset} from "jstests/noPassthroughWithMongod/query/cbr/lib/datasets/number.js";
import {StringDataset} from "jstests/noPassthroughWithMongod/query/cbr/lib/datasets/string.js";

// TODO SERVER-92589: Remove this exemption
if (checkSbeFullyEnabled(db)) {
    jsTestLog(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

const collName = jsTestName();
const coll = db[collName];

function runOneTest({dataset, indexes, analyze, numberBuckets = 1000}) {
    try {
        assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "histogramCE"}));

        coll.drop();
        assert.commandWorked(coll.insertMany(dataset.docs()));

        for (const index of indexes ? indexes : []) {
            assert.commandWorked(coll.createIndex(index));
        }

        for (const analyze_key of analyze ? analyze : ["a"]) {
            var analyze_cmd = {analyze: collName, key: analyze_key, numberBuckets: numberBuckets};

            assert.commandWorked(coll.runCommand(analyze_cmd));
        }

        for (const predicate of dataset.predicates()) {
            var cursor = coll.find(predicate);
            const actualDocuments = cursor.count();

            const explain = cursor.explain();
            const plans = getAllPlans(explain);
            for (const plan of plans) {
                assert(plan.hasOwnProperty("cardinalityEstimate"));
                const cardinalityEstimate = plan.cardinalityEstimate;

                // 'Histogram', 'Code' and 'Metadata' all imply a confident estimate,
                // so we accept all of them.
                assert(plan.estimatesMetadata.ceSource === "Histogram" ||
                           plan.estimatesMetadata.ceSource === "Code" ||
                           plan.estimatesMetadata.ceSource === "Metadata",
                       predicate);

                printjsononeline(predicate);
                print(`actualDocuments: ${actualDocuments}; cardinalityEstimate: ${
                    cardinalityEstimate}`);

                if (Math.abs(actualDocuments - cardinalityEstimate) > 1) {
                    printjsononeline(plan);
                    assert(
                        false,
                        `Got cardinalityEstimate = ${cardinalityEstimate} but actualDocuments = ${
                            actualDocuments} for predicate: ${tojson(predicate)}; dataset: ${
                            dataset.constructor.name}; indexes: ${indexes};`);
                }
            }
        }
    } finally {
        // Make sure that we restore the default no matter what
        assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
    }
}

for (const indexes of [[], [{a: 1}]]) {
    for (const dataset of [new ArrayDataset(),
                           new BooleanDataset(),
                           new DateDataset(),
                           new TimestampDataset(),
                           new SkewedDataset(),
                           new MixedTypesDataset(),
                           new MixedNumbersDataset(),
                           new NumberDataset(),
                           new StringDataset()]) {
        runOneTest({dataset: dataset, indexes: indexes});
    }

    /* Skewed datasets under a constrained number of buckets. We give each
       dataset just enough buckets for it can be estimated accurately.
    */
    for (const test of [{dataset: new UniformDataset(), numberBuckets: 2},
                        {dataset: new OnePeakDataset(), numberBuckets: 4},
                        {dataset: new OneHoleDataset(), numberBuckets: 3},
                        {dataset: new ThreePeakDataset(), numberBuckets: 8},
                        {dataset: new SkewedDataset(), numberBuckets: 10}]) {
        test.indexes = indexes;
        runOneTest(test);
    }
}

// Multi-field predicates

for (const indexes of [[{a: 1, b: 1}], [{a: 1}, {b: 1}]]) {
    runOneTest({dataset: new TwoFieldDataset(), indexes: indexes, analyze: ["a", "b"]});
}
