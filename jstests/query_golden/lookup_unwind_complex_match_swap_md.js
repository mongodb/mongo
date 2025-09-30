/**
 * Tests that when the parameter internalQueryPermitMatchSwappingForComplexRenames is set,
 * then match will get pushed down into $lookup/$unwind.
 *
 * This emulates a use case in which an application with a relational schema defines views which
 * use $lookup-$unwind to join several tables. Then predicates may be applied on top of the view.
 * In this case, we want to make sure that the predicates are pushed down as far as possible to
 * the appropriate base collections of the view.
 */

import {normalizeArray} from "jstests/libs/golden_test.js";
import {code, linebreak, section, subSection} from "jstests/libs/pretty_md.js";

try {
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQueryPermitMatchSwappingForComplexRenames: true}));

    const coll_a = db.lu_complex_swap_a;
    const coll_b = db.lu_complex_swap_b;
    const coll_c = db.lu_complex_swap_c;
    const view = db.lu_complex_swap_view;

    coll_a.drop();
    coll_b.drop();
    coll_c.drop();
    view.drop();

    const coll_a_name = coll_a.getName();
    const coll_b_name = coll_b.getName();
    const coll_c_name = coll_c.getName();
    const view_name = view.getName();

    section('Inserting docs into collection "a":');

    const a_docs = [
        {_id: 1, b: 4, my_id: 100, m: {c: 42}},
        {_id: 2, b: 4, my_id: 101, m: {}},
        {_id: 3, b: 4, my_id: 100},
        {_id: 4, b: 4, m: {c: null}},
        {_id: 5, b: 4, m: {c: 42, d: "foo"}},
    ];
    code(tojson(a_docs));

    assert.commandWorked(coll_a.insert(a_docs));

    section('Inserting docs into collection "b":');

    const b_docs = [
        {_id: 1, b: 4, indicator: "X"},
        {_id: 2, b: 4, indicator: "Y"},
        {_id: 3, b: 4},
        {_id: 4, b: 4, indicator: {"Z": "Y"}},
        {_id: 5, b: 4, indicator: "Z"},
    ];
    code(tojson(b_docs));

    assert.commandWorked(coll_b.insert(b_docs));

    section('Inserting docs into collection "c":');

    const c_docs = [
        {_id: 1, b: 4, code: "X"},
        {_id: 2, b: 4, other_id: 42, code: "bar"},
        {_id: 3, b: 4, other_id: 42},
        {_id: 4, b: 4, code: "blah"},
        {_id: 5, b: 4, other_id: 20, code: "foo"},
        {_id: 6, b: 4, other_id: {zip: 42, zap: 20}, code: "bar"},
        {_id: 7, b: 4, other_id: {zip: 20, zap: 42}},
    ];
    code(tojson(c_docs));

    assert.commandWorked(coll_c.insert(c_docs));

    function runFindOnPipeline(pipeline, queries) {
        section("View pipeline");
        code(tojson(pipeline));

        // Append {$_internalInhibitOptimization: {}} to the front of the pipeline. This prevents
        // pushdown into the find layer, which means that we can just print the pipeline (without
        // $cursor) to the golden file.
        pipeline.unshift({$_internalInhibitOptimization: {}});

        view.drop();
        assert.commandWorked(db.createView(view_name, coll_a_name, pipeline));

        for (let query of queries) {
            subSection("Query");
            code(tojsononeline(query));

            // Print the results of the query to the golden file.
            subSection("Results");
            code(normalizeArray(view.find(query).toArray()));

            let explain = view.find(query).explain("queryPlanner");
            // Since we prevented pushdown into the find layer, we expect an array of pipeline
            // stages to be present in the explain output.
            assert(explain.hasOwnProperty("stages"), explain);

            // Drop the first two stages, since we don't need to see the $cursor or
            // $_inhibitOptimization in the golden output.
            let stages = explain.stages;
            assert.gte(stages.length, 3, explain);
            stages = stages.slice(2);
            subSection("Explain");
            code(tojson(stages));
            linebreak();
        }
    }

    let pipeline = [
        {$match: {my_id: 100}},
        {$lookup: {from: coll_b_name, as: "B_data", localField: "b", foreignField: "b"}},
        {$unwind: "$B_data"},
        {$match: {"B_data.indicator": "Y"}},
        {$lookup: {from: coll_c_name, as: "C_data", localField: "b", foreignField: "b"}},
        {$unwind: "$C_data"},
        {
            $addFields: {
                other_id: "$C_data.other_id",
            },
        },
    ];

    let queries = [{other_id: 42}, {"other_id.zip": 42}];

    runFindOnPipeline(pipeline, queries);

    pipeline = [
        {$match: {my_id: 100}},
        {$lookup: {from: coll_b_name, as: "B_data", localField: "b", foreignField: "b"}},
        {$unwind: "$B_data"},
        {$match: {"B_data.indicator": "Y"}},
        {$lookup: {from: coll_c_name, as: "C_data", localField: "b", foreignField: "b"}},
        {$unwind: "$C_data"},
        {
            $addFields: {
                zip: "$C_data.other_id.zip",
            },
        },
    ];

    // We only support "complex renames" where the field path is 2 components long. In this case,
    // the field path has three components, so we don't expect the match to be pushed down.
    queries = [{zip: 42}];
    runFindOnPipeline(pipeline, queries);

    pipeline = [
        {$match: {my_id: 100}},
        {$lookup: {from: coll_b_name, as: "B_data", localField: "b", foreignField: "b"}},
        {$unwind: "$B_data"},
        {$match: {"B_data.indicator": "Y"}},
        {$lookup: {from: coll_c_name, as: "C_data", localField: "b", foreignField: "b"}},
        {$unwind: "$C_data"},
        {$project: {_id: 1, other_id: "$C_data.other_id", code: 1}},
    ];

    queries = [{other_id: 42}, {"other_id.zip": 42}];

    runFindOnPipeline(pipeline, queries);

    pipeline = [
        {$match: {my_id: 100}},
        {$lookup: {from: coll_b_name, as: "B_data", localField: "b", foreignField: "b"}},
        {$unwind: "$B_data"},
        {$match: {"B_data.indicator": "Y"}},
        {$lookup: {from: coll_c_name, as: "C_data", localField: "b", foreignField: "b"}},
        {$unwind: "$C_data"},
        {$project: {_id: 1, zip: "$C_data.other_id.zip", code: 1}},
    ];

    // Like above, the renamed path is 3 components long, so we don't expect the match to be pushed
    // down.
    queries = [{zip: 42}];
    runFindOnPipeline(pipeline, queries);

    pipeline = [
        {$match: {my_id: 100}},
        {$lookup: {from: coll_b_name, as: "B_data", localField: "b", foreignField: "b"}},
        {$unwind: "$B_data"},
        {$match: {"B_data.indicator": "Y"}},
        {$lookup: {from: coll_c_name, as: "C_data", localField: "b", foreignField: "b"}},
        {$unwind: "$C_data"},
        {$project: {_id: 0, indicator: "$B_data.indicator", code: "$C_data.code"}},
    ];

    // In this case, the match should be pushed down through the rename done by the $project. Then
    // it should be pushed down past the first second $lookup-$unwind pair and into the subpipeline
    // of the first $lookup-$unwind pair.
    queries = [{"indicator.Z": "Y"}];
    runFindOnPipeline(pipeline, queries);
} finally {
    // Reset the parameter to its default value.
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQueryPermitMatchSwappingForComplexRenames: false}));
}
