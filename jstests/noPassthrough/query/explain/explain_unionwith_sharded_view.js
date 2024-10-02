/**
 * Test that explain of $unionWith resolves views on sharded collections correctly.
 */
import {
    assertDropAndRecreateCollection,
    assertDropCollection
} from "jstests/libs/collection_drop_recreate.js";
import {getAllNodeExplains} from "jstests/libs/query/analyze_plan.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";

const st = new ShardingTest({shards: 2});
const db = st.s.getDB(dbName);

function setupColl(name) {
    const coll = assertDropAndRecreateCollection(db, name);
    coll.insertMany([{a: 1, b: 5}, {a: "foo", b: "bar"}, {a: "foo1", b: "bar1"}]);
    st.shardColl(coll.getName(), {_id: 1});
    return coll;
}

function setupView(name, sourceName) {
    assertDropCollection(db, name);
    assert.commandWorked(db.createView(name, sourceName, []));
    return name;
}

const coll = setupColl(jsTestName());
const viewOnColl = setupView("viewOnColl", coll.getName());

const secondaryColl = setupColl(jsTestName() + "_secondary");
const viewOnSecondaryColl = setupView("viewOnSecondaryColl", secondaryColl.getName());

// Each view is backed by the view that precedes it in the list.
const nestedViews = [viewOnSecondaryColl];
for (let i = 1; i < 10; i++) {
    const nestedView = setupView("nestedView" + i, nestedViews[i - 1]);
    nestedViews.push(nestedView);
}

function testPipeline(pipeline) {
    print("Testing pipeline:", tojson(pipeline));

    const checkExplainProperties = (res, hasExecutionStats) => {
        const errorMsg = tojson(res);
        assert.eq(res.mergeType, "anyShard", errorMsg);

        const {shardsPart} = res.splitPipeline;
        assert(!shardsPart.some(x => x.hasOwnProperty("$unionWith")), errorMsg);

        const {mergerPart} = res.splitPipeline;
        assert(mergerPart.some(x => x.hasOwnProperty("$unionWith")), errorMsg);
        assert(mergerPart.some(x => x.hasOwnProperty("$mergeCursors")), errorMsg);

        if (hasExecutionStats) {
            const allExecutionStats = [];
            for (const nodeExplain of getAllNodeExplains(res)) {
                assert(nodeExplain.hasOwnProperty("executionStats"));
                allExecutionStats.push(nodeExplain.executionStats);
            }

            // Each $unionWith matches one document. There are two shards, so every other explain
            // node should match a document.
            assert(allExecutionStats.filter(x => x.nReturned > 1).length === 0);
            assert(allExecutionStats.filter(x => x.nReturned === 1).length ===
                   allExecutionStats.filter(x => x.nReturned === 0).length);
        }
    };

    jsTestLog("Running explain on collection:");
    checkExplainProperties(coll.explain().aggregate(pipeline));

    jsTestLog("Running explain on view:");
    checkExplainProperties(db[viewOnColl].explain().aggregate(pipeline));

    jsTestLog("Running explain('executionStats') on collection:");
    checkExplainProperties(coll.explain("executionStats").aggregate(pipeline), true);

    jsTestLog("Running explain('executionStats') on view:");
    checkExplainProperties(db[viewOnColl].explain("executionStats").aggregate(pipeline), true);
}

const testViews = [
    nestedViews[0],  // View depth 1
    nestedViews[4],  // View depth 5
    nestedViews[9],  // View depth 10
];

// Single $unionWith on a sharded view.
for (const viewName of testViews) {
    testPipeline([
        {$match: {a: "foo", b: "bar"}},
        {$unionWith: {coll: viewName, pipeline: [{$match: {a: 1, b: 5}}]}},
    ]);
}

// Multiple $unionWith stages on sharded views.
testPipeline([
    {$match: {a: "foo", b: "bar"}},
    ...testViews.map(view => ({$unionWith: {coll: view, pipeline: [{$match: {a: 1, b: 5}}]}})),
]);

// Nested $unionWith on sharded views.
const [firstView, secondView, thirdView, ..._] = testViews;
testPipeline([
    {$match: {a: "foo", b: "bar"}},
    {
        $unionWith: {
            coll: firstView,
            pipeline: [
                {$match: {a: 1, b: 5}},
                {
                    $unionWith: {
                        coll: secondView,
                        pipeline: [
                            {$match: {a: 1, b: 5}},
                            {$unionWith: {coll: thirdView, pipeline: [{$match: {a: 1, b: 5}}]}}
                        ]
                    }
                }
            ]
        }
    }
]);

st.stop();
