// Tests the behavior of queries with a {$eq: null} or {$ne: null} predicate.
// @tags: [
//   # Columnstore tests set server parameters to disable columnstore query planning heuristics -
//   # 1) server parameters are stored in-memory only so are not transferred onto the recipient,
//   # 2) server parameters may not be set in stepdown passthroughs because it is a command that may
//   #      return different values after a failover
//   tenant_migration_incompatible,
//   does_not_support_stepdowns,
//   not_allowed_with_signed_security_token,
//   # In 8.0, we changed behavior for equality to null.
//   requires_fcv_80,
// ]
//
import {resultsEq} from "jstests/aggregation/extras/utils.js";
import {setUpServerForColumnStoreIndexTest} from "jstests/libs/columnstore_util.js";

function extractAValues(results) {
    return results.map(function(res) {
        if (!res.hasOwnProperty("a")) {
            return {};
        }
        return {a: res.a};
    });
}

function testNullSemantics(coll) {
    // For the first portion of the test, only insert documents without arrays. This will avoid
    // making the indexes multi-key, which may allow an index to be used to answer the queries.
    assert.commandWorked(coll.insert([
        {_id: "a_empty_subobject", a: {}},
        {_id: "a_null", a: null},
        {_id: "a_number", a: 4},
        {_id: "a_subobject_b_not_null", a: {b: "hi"}},
        {_id: "a_subobject_b_null", a: {b: null}},
        {_id: "a_subobject_b_undefined", a: {b: undefined}},
        {_id: "a_undefined", a: undefined},
        {_id: "no_a"},
    ]));

    // Throughout this test we will run queries with a projection which may allow the planner to
    // consider an index-only plan. Checking the results of those queries will test that the
    // query system will never choose such an optimization if it is incorrect.
    const projectToOnlyA = {_id: 0, a: 1};
    const projectToOnlyADotB = {_id: 0, "a.b": 1};

    // Test the semantics of the query {a: {$eq: null}}.
    (function testBasicNullQuery() {
        const noProjectResults = coll.find({a: {$eq: null}}).toArray();
        const expected = [{_id: "a_null", a: null}, {_id: "no_a"}];
        assert(resultsEq(expected, noProjectResults), tojson(noProjectResults));

        const count = coll.count({a: {$eq: null}});
        assert.eq(count, expected.length);

        const projectResults = coll.find({a: {$eq: null}}, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), tojson(projectResults));
    }());

    // Test the semantics of the query {a: {$ne: null}}.
    (function testBasicNotEqualsNullQuery() {
        const noProjectResults = coll.find({a: {$ne: null}}).toArray();
        const expected = [
            {_id: "a_undefined", a: undefined},
            {_id: "a_empty_subobject", a: {}},
            {_id: "a_number", a: 4},
            {_id: "a_subobject_b_not_null", a: {b: "hi"}},
            {_id: "a_subobject_b_null", a: {b: null}},
            {_id: "a_subobject_b_undefined", a: {b: undefined}},
        ];
        assert(resultsEq(noProjectResults, expected), tojson(noProjectResults));

        const count = coll.count({a: {$ne: null}});
        assert.eq(count, expected.length);

        const projectResults = coll.find({a: {$ne: null}}, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), tojson(projectResults));
    }());

    // Test the semantics of the query {a: {$in: [null, <number>]}}.
    (function testInNullQuery() {
        const query = {a: {$in: [null, 4]}};
        const noProjectResults = coll.find(query).toArray();
        const expected = [
            {_id: "a_null", a: null},
            {_id: "a_number", a: 4},
            {_id: "no_a"},
        ];

        const count = coll.count(query);
        assert.eq(count, expected.length);

        assert(resultsEq(noProjectResults, expected), noProjectResults);

        const projectResults = coll.find(query, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), projectResults);
    }());

    // Test the semantics of the query {$or: [{a: null}, {a: <number>}]}.
    (function testOrNullQuery() {
        const query = {$or: [{a: null}, {a: 4}]};
        const noProjectResults = coll.find(query).toArray();
        const expected = [
            {_id: "a_null", a: null},
            {_id: "a_number", a: 4},
            {_id: "no_a"},
        ];

        const count = coll.count(query);
        assert.eq(count, expected.length);

        assert(resultsEq(noProjectResults, expected), noProjectResults);

        const projectResults = coll.find(query, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), projectResults);
    }());

    // Test the semantics of the query {a: {$nin: [null, <number>]}}.
    (function testNotInNullQuery() {
        const query = {a: {$nin: [null, 4]}};
        const noProjectResults = coll.find(query).toArray();
        const expected = [
            {_id: "a_undefined", a: undefined},
            {_id: "a_empty_subobject", a: {}},
            {_id: "a_subobject_b_not_null", a: {b: "hi"}},
            {_id: "a_subobject_b_null", a: {b: null}},
            {_id: "a_subobject_b_undefined", a: {b: undefined}},
        ];

        const count = coll.count(query);
        assert.eq(count, expected.length);

        assert(resultsEq(noProjectResults, expected), noProjectResults);

        const projectResults = coll.find(query, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), projectResults);
    }());

    (function testNotInNullAndRegexQuery() {
        // While $nin: [null, ...] can be indexed, $nin: [<regex>] cannot. Ensure that we get
        // the correct results in this case.
        const query = {a: {$nin: [null, /^hi.*/]}};
        const noProjectResults = coll.find(query).toArray();
        const expected = [
            {_id: "a_undefined", a: undefined},
            {_id: "a_empty_subobject", a: {}},
            {_id: "a_number", a: 4},
            {_id: "a_subobject_b_not_null", a: {b: "hi"}},
            {_id: "a_subobject_b_null", a: {b: null}},
            {_id: "a_subobject_b_undefined", a: {b: undefined}},
        ];
        assert(resultsEq(noProjectResults, expected), tojson(noProjectResults));

        const count = coll.count(query);
        assert.eq(count, expected.length);

        const projectResults = coll.find(query, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), projectResults);
    }());

    (function testExistsFalse() {
        const noProjectResults = coll.find({a: {$exists: false}}).toArray();
        const expected = [
            {_id: "no_a"},
        ];
        assert(resultsEq(noProjectResults, expected), tojson(noProjectResults));

        const count = coll.count({a: {$exists: false}});
        assert.eq(count, expected.length);

        const projectResults = coll.find({a: {$exists: false}}, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), tojson(projectResults));
    }());

    // Test the semantics of the query {"a.b": {$eq: null}}.
    (function testDottedEqualsNull() {
        const noProjectResults = coll.find({"a.b": {$eq: null}}).toArray();
        const expected = [
            {_id: "a_empty_subobject", a: {}},
            {_id: "a_null", a: null},
            {_id: "a_number", a: 4},
            {_id: "a_subobject_b_null", a: {b: null}},
            {_id: "a_undefined", a: undefined},
            {_id: "no_a"}
        ];
        assert(resultsEq(noProjectResults, expected), tojson(noProjectResults));

        const count = coll.count({"a.b": {$eq: null}});
        assert.eq(count, expected.length);

        const projectResults = coll.find({"a.b": {$eq: null}}, projectToOnlyADotB).toArray();
        assert(resultsEq(projectResults, [{a: {}}, {}, {}, {a: {b: null}}, {}, {}]),
               tojson(projectResults));
    }());

    // Test the semantics of the query {"a.b": {$ne: null}}.
    (function testDottedNotEqualsNull() {
        const noProjectResults = coll.find({"a.b": {$ne: null}}).toArray();
        const expected = [
            {_id: "a_subobject_b_not_null", a: {b: "hi"}},
            {_id: "a_subobject_b_undefined", a: {b: undefined}}
        ];
        assert(resultsEq(noProjectResults, expected), tojson(noProjectResults));

        const count = coll.count({"a.b": {$ne: null}});
        assert.eq(count, expected.length);

        const projectResults = coll.find({"a.b": {$ne: null}}, projectToOnlyADotB).toArray();
        assert(resultsEq(projectResults, [{a: {b: "hi"}}, {a: {b: undefined}}]),
               tojson(projectResults));
    }());

    (function testDottedExistsFalse() {
        const noProjectResults = coll.find({"a.b": {$exists: false}}).toArray();
        const expected = [
            {_id: "no_a"},
            {_id: "a_empty_subobject", a: {}},
            {_id: "a_null", a: null},
            {_id: "a_number", a: 4},
            {_id: "a_undefined", a: undefined},
        ];
        assert(resultsEq(noProjectResults, expected), tojson(noProjectResults));

        const count = coll.count({"a.b": {$exists: false}});
        assert.eq(count, expected.length);

        const projectResults = coll.find({"a.b": {$exists: false}}, projectToOnlyADotB).toArray();
        assert(resultsEq(projectResults, [{}, {a: {}}, {}, {}, {}]), tojson(projectResults));
    }());

    // Test similar queries, but with an $elemMatch. These queries should have no results since
    // an $elemMatch requires an array.
    (function testElemMatchQueriesWithNoArrays() {
        for (let elemMatchQuery of [{a: {$elemMatch: {$eq: null}}},
                                    {a: {$elemMatch: {$ne: null}}},
                                    {"a.b": {$elemMatch: {$eq: null}}},
                                    {"a.b": {$elemMatch: {$ne: null}}},
                                    {a: {$elemMatch: {b: {$eq: null}}}},
                                    {a: {$elemMatch: {b: {$ne: null}}}},
        ]) {
            const noProjectResults = coll.find(elemMatchQuery).toArray();
            assert(resultsEq(noProjectResults, []),
                   `Expected no results for query ${tojson(elemMatchQuery)}, got ` +
                       tojson(noProjectResults));

            const count = coll.count(elemMatchQuery);
            assert.eq(count, 0);

            let projectResults = coll.find(elemMatchQuery, projectToOnlyA).toArray();
            assert(resultsEq(projectResults, []),
                   `Expected no results for query ${tojson(elemMatchQuery)}, got ` +
                       tojson(projectResults));

            projectResults = coll.find(elemMatchQuery, projectToOnlyADotB).toArray();
            assert(resultsEq(projectResults, []),
                   `Expected no results for query ${tojson(elemMatchQuery)}, got ` +
                       tojson(projectResults));
        }
    }());

    // An index which includes "a" or a sub-path of "a" will become multi-key after this insert.
    const writeResult = coll.insert([
        {_id: "a_double_array", a: [[]]},
        {_id: "a_empty_array", a: []},
        {
            _id: "a_object_array_b_mix_null_undefined_missing",
            a: [{b: null}, {b: undefined}, {b: null}, {}]
        },
        {_id: "a_object_array_no_b_nulls", a: [{b: 1}, {b: 3}, {b: "string"}]},
        {_id: "a_object_array_some_b_nulls", a: [{b: null}, {b: 3}, {b: null}]},
        {_id: "a_object_array_some_b_undefined", a: [{b: undefined}, {b: 3}]},
        {_id: "a_object_array_some_b_missing", a: [{b: 3}, {}]},
        {_id: "a_value_array_all_nulls", a: [null, null]},
        {_id: "a_value_array_no_nulls", a: [1, "string", 4]},
        {_id: "a_value_array_with_null", a: [1, "string", null, 4]},
        {_id: "a_value_array_with_undefined", a: [1, "string", undefined, 4]},
    ]);
    if (writeResult.hasWriteErrors()) {
        // We're testing a hashed index which is incompatible with arrays. Skip the multi-key
        // portion of this test for this index.
        assert.eq(writeResult.getWriteErrors().length, 1, tojson(writeResult));
        assert.eq(writeResult.getWriteErrors()[0].code, 16766, tojson(writeResult));
        return;
    }
    assert.commandWorked(writeResult);

    // Test the semantics of the query {a: {$eq: null}}.
    (function testBasicNullQuery() {
        const noProjectResults = coll.find({a: {$eq: null}}).toArray();
        const expected = [
            {_id: "a_null", a: null},
            {_id: "a_value_array_all_nulls", a: [null, null]},
            {_id: "a_value_array_with_null", a: [1, "string", null, 4]},
            {_id: "no_a"},
        ];
        assert(resultsEq(noProjectResults, expected), tojson(noProjectResults));

        const count = coll.count({a: {$eq: null}});
        assert.eq(count, expected.length);

        const projectResults = coll.find({a: {$eq: null}}, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), tojson(projectResults));
    }());

    // Test the semantics of the query {a: {$ne: null}}.
    (function testBasicNotEqualsNullQuery() {
        const noProjectResults = coll.find({a: {$ne: null}}).toArray();
        const expected = [
            {_id: "a_undefined", a: undefined},
            {_id: "a_double_array", a: [[]]},
            {_id: "a_empty_array", a: []},
            {_id: "a_empty_subobject", a: {}},
            {_id: "a_number", a: 4},
            {
                _id: "a_object_array_b_mix_null_undefined_missing",
                a: [{b: null}, {b: undefined}, {b: null}, {}]
            },
            {_id: "a_object_array_no_b_nulls", a: [{b: 1}, {b: 3}, {b: "string"}]},
            {_id: "a_object_array_some_b_nulls", a: [{b: null}, {b: 3}, {b: null}]},
            {_id: "a_object_array_some_b_undefined", a: [{b: undefined}, {b: 3}]},
            {_id: "a_object_array_some_b_missing", a: [{b: 3}, {}]},
            {_id: "a_subobject_b_not_null", a: {b: "hi"}},
            {_id: "a_subobject_b_null", a: {b: null}},
            {_id: "a_subobject_b_undefined", a: {b: undefined}},
            {_id: "a_value_array_no_nulls", a: [1, "string", 4]},
            {_id: "a_value_array_with_undefined", a: [1, "string", undefined, 4]},
        ];
        assert(resultsEq(noProjectResults, expected), tojson(noProjectResults));

        const count = coll.count({a: {$ne: null}});
        assert.eq(count, expected.length);

        const projectResults = coll.find({a: {$ne: null}}, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), tojson(projectResults));
    }());

    // Test the semantics of the query {a: {$not: {$gte: null}}}.
    (function testBasicNotEqualsGTENullQuery() {
        const noProjectResults = coll.find({a: {$not: {$gte: null}}}).toArray();
        const expected = [
            {_id: "a_undefined", a: undefined},
            {_id: "a_double_array", a: [[]]},
            {_id: "a_empty_array", a: []},
            {_id: "a_empty_subobject", a: {}},
            {_id: "a_number", a: 4},
            {
                _id: "a_object_array_b_mix_null_undefined_missing",
                a: [{b: null}, {b: undefined}, {b: null}, {}]
            },
            {_id: "a_object_array_no_b_nulls", a: [{b: 1}, {b: 3}, {b: "string"}]},
            {_id: "a_object_array_some_b_nulls", a: [{b: null}, {b: 3}, {b: null}]},
            {_id: "a_object_array_some_b_undefined", a: [{b: undefined}, {b: 3}]},
            {_id: "a_object_array_some_b_missing", a: [{b: 3}, {}]},
            {_id: "a_subobject_b_not_null", a: {b: "hi"}},
            {_id: "a_subobject_b_null", a: {b: null}},
            {_id: "a_subobject_b_undefined", a: {b: undefined}},
            {_id: "a_value_array_no_nulls", a: [1, "string", 4]},
            {_id: "a_value_array_with_undefined", a: [1, "string", undefined, 4]},
        ];
        assert(resultsEq(noProjectResults, expected), tojson(noProjectResults));
        const count = coll.count({a: {$not: {$gte: null}}});
        assert.eq(count, expected.length);
    }());

    // Test the semantics of the query {a: {$in: [null, <number>]}}.
    (function testInNullQuery() {
        const query = {a: {$in: [null, 75]}};
        const noProjectResults = coll.find(query).toArray();
        const expected = [
            {_id: "a_null", a: null},
            {_id: "no_a"},
            {_id: "a_value_array_all_nulls", a: [null, null]},
            {_id: "a_value_array_with_null", a: [1, "string", null, 4]},
        ];

        assert(resultsEq(noProjectResults, expected), noProjectResults);

        const count = coll.count({a: {$in: [null, 75]}});
        assert.eq(count, expected.length);

        const projectResults = coll.find(query, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), projectResults);
    }());

    // Test the semantics of the query {$or: [{a: null}, {a: <number>}]}.
    (function testOrNullQuery() {
        const query = {$or: [{a: null}, {a: 75}]};
        const noProjectResults = coll.find(query).toArray();
        const expected = [
            {_id: "a_null", a: null},
            {_id: "no_a"},
            {_id: "a_value_array_all_nulls", a: [null, null]},
            {_id: "a_value_array_with_null", a: [1, "string", null, 4]},
        ];

        assert(resultsEq(noProjectResults, expected), noProjectResults);

        const count = coll.count({$or: [{a: null}, {a: 75}]});
        assert.eq(count, expected.length);

        const projectResults = coll.find(query, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), projectResults);
    }());

    // Test the semantics of the query {a: {$nin: [null, <number>]}}.
    (function testNotInNullQuery() {
        const query = {a: {$nin: [null, 75]}};
        const noProjectResults = coll.find(query).toArray();
        const expected = [
            {_id: "a_undefined", a: undefined},
            {_id: "a_empty_subobject", a: {}},
            {_id: "a_number", a: 4},
            {_id: "a_subobject_b_not_null", a: {b: "hi"}},
            {_id: "a_subobject_b_null", a: {b: null}},
            {_id: "a_subobject_b_undefined", a: {b: undefined}},

            {_id: "a_double_array", a: [[]]},
            {_id: "a_empty_array", a: []},
            {
                _id: "a_object_array_b_mix_null_undefined_missing",
                a: [{b: null}, {b: undefined}, {b: null}, {}]
            },
            {_id: "a_object_array_no_b_nulls", a: [{b: 1}, {b: 3}, {b: "string"}]},
            {_id: "a_object_array_some_b_nulls", a: [{b: null}, {b: 3}, {b: null}]},
            {_id: "a_object_array_some_b_undefined", a: [{b: undefined}, {b: 3}]},
            {_id: "a_object_array_some_b_missing", a: [{b: 3}, {}]},
            {_id: "a_value_array_no_nulls", a: [1, "string", 4]},
            {_id: "a_value_array_with_undefined", a: [1, "string", undefined, 4]},
        ];

        assert(resultsEq(noProjectResults, expected), noProjectResults);

        const count = coll.count({a: {$nin: [null, 75]}});
        assert.eq(count, expected.length);

        const projectResults = coll.find(query, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), projectResults);
    }());

    // Test the semantics of the query {a: {$nin: [null, []]}}.
    (function testNotInNullAndEmptyArrayQuery() {
        const query = {a: {$nin: [null, []]}};
        const noProjectResults = coll.find(query).toArray();
        const expected = [
            // Documents excluded by the query predicate are commented
            // out below.
            {_id: "a_empty_subobject", a: {}},
            //{_id: "a_null", a: null},
            {_id: "a_number", a: 4},
            {_id: "a_subobject_b_not_null", a: {b: "hi"}},
            {_id: "a_subobject_b_null", a: {b: null}},
            {_id: "a_subobject_b_undefined", a: {b: undefined}},
            {_id: "a_undefined", a: undefined},
            //{_id: "no_a"},

            //{_id: "a_double_array", a: [[]]},
            //{_id: "a_empty_array", a: []},
            {
                _id: "a_object_array_b_mix_null_undefined_missing",
                a: [{b: null}, {b: undefined}, {b: null}, {}]
            },
            {_id: "a_object_array_no_b_nulls", a: [{b: 1}, {b: 3}, {b: "string"}]},
            {_id: "a_object_array_some_b_nulls", a: [{b: null}, {b: 3}, {b: null}]},
            {_id: "a_object_array_some_b_undefined", a: [{b: undefined}, {b: 3}]},
            {_id: "a_object_array_some_b_missing", a: [{b: 3}, {}]},
            //{_id: "a_value_array_all_nulls", a: [null, null]},
            {_id: "a_value_array_no_nulls", a: [1, "string", 4]},
            //{_id: "a_value_array_with_null", a: [1, "string", null, 4]},
            {_id: "a_value_array_with_undefined", a: [1, "string", undefined, 4]},
        ];

        assert(resultsEq(noProjectResults, expected), noProjectResults);

        const count = coll.count({a: {$nin: [null, []]}});
        assert.eq(count, expected.length);

        const projectResults = coll.find(query, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), projectResults);
    }());

    (function testNotInNullAndRegexQuery() {
        const query = {a: {$nin: [null, /^str.*/]}};
        const noProjectResults = coll.find(query).toArray();
        const expected = [
            {_id: "a_undefined", a: undefined},
            {_id: "a_empty_subobject", a: {}},
            {_id: "a_number", a: 4},
            {_id: "a_subobject_b_not_null", a: {b: "hi"}},
            {_id: "a_subobject_b_null", a: {b: null}},
            {_id: "a_subobject_b_undefined", a: {b: undefined}},

            {_id: "a_double_array", a: [[]]},
            {_id: "a_empty_array", a: []},
            {
                _id: "a_object_array_b_mix_null_undefined_missing",
                a: [{b: null}, {b: undefined}, {b: null}, {}]
            },
            {_id: "a_object_array_no_b_nulls", a: [{b: 1}, {b: 3}, {b: "string"}]},
            {_id: "a_object_array_some_b_nulls", a: [{b: null}, {b: 3}, {b: null}]},
            {_id: "a_object_array_some_b_undefined", a: [{b: undefined}, {b: 3}]},
            {_id: "a_object_array_some_b_missing", a: [{b: 3}, {}]},
        ];

        assert(resultsEq(noProjectResults, expected), noProjectResults);

        const count = coll.count(query);
        assert.eq(count, expected.length);

        const projectResults = coll.find(query, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), projectResults);
    }());

    // Test the results of similar queries with an $elemMatch.
    (function testElemMatchValue() {
        // Test $elemMatch with equality to null.
        let noProjectResults = coll.find({a: {$elemMatch: {$eq: null}}}).toArray();
        const expectedEqualToNull = [
            {_id: "a_value_array_all_nulls", a: [null, null]},
            {_id: "a_value_array_with_null", a: [1, "string", null, 4]},
        ];
        assert(resultsEq(noProjectResults, expectedEqualToNull), tojson(noProjectResults));

        const count = coll.count({a: {$elemMatch: {$eq: null}}});
        assert.eq(count, expectedEqualToNull.length);

        let projectResults = coll.find({a: {$elemMatch: {$eq: null}}}, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expectedEqualToNull)),
               tojson(projectResults));

        // Test $elemMatch with not equal to null.
        noProjectResults = coll.find({a: {$elemMatch: {$ne: null}}}).toArray();
        const expectedNotEqualToNull = [
            {_id: "a_double_array", a: [[]]},
            {
                _id: "a_object_array_b_mix_null_undefined_missing",
                a: [{b: null}, {b: undefined}, {b: null}, {}]
            },
            {_id: "a_object_array_no_b_nulls", a: [{b: 1}, {b: 3}, {b: "string"}]},
            {_id: "a_object_array_some_b_nulls", a: [{b: null}, {b: 3}, {b: null}]},
            {_id: "a_object_array_some_b_undefined", a: [{b: undefined}, {b: 3}]},
            {_id: "a_object_array_some_b_missing", a: [{b: 3}, {}]},
            {_id: "a_value_array_no_nulls", a: [1, "string", 4]},
            {_id: "a_value_array_with_undefined", a: [1, "string", undefined, 4]},
            {_id: "a_value_array_with_null", a: [1, "string", null, 4]},
        ];
        assert(resultsEq(noProjectResults, expectedNotEqualToNull), tojson(noProjectResults));

        projectResults = coll.find({a: {$elemMatch: {$ne: null}}}, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expectedNotEqualToNull)),
               tojson(projectResults));
    }());

    // Test the semantics of the query {"a.b": {$eq: null}}. The semantics here are to return
    // those documents which have one of the following properties:
    //  - A non-object, non-array value for "a"
    //  - A subobject "a" with a missing, null, or undefined value for "b"
    //  - An array which has at least one object in it which has a missing, null, or undefined
    //    value for "b".
    (function testDottedEqualsNull() {
        const noProjectResults = coll.find({"a.b": {$eq: null}}).toArray();
        const expected = [
            {_id: "a_empty_subobject", a: {}},
            {_id: "a_null", a: null},
            {_id: "a_number", a: 4},
            {_id: "a_subobject_b_null", a: {b: null}},
            {_id: "a_undefined", a: undefined},
            {_id: "no_a"},
            {
                _id: "a_object_array_b_mix_null_undefined_missing",
                a: [{b: null}, {b: undefined}, {b: null}, {}]
            },
            {_id: "a_object_array_some_b_nulls", a: [{b: null}, {b: 3}, {b: null}]},
            {_id: "a_object_array_some_b_missing", a: [{b: 3}, {}]},
        ];
        assert(resultsEq(noProjectResults, expected), tojson(noProjectResults));

        const count = coll.count({"a.b": {$eq: null}});
        assert.eq(count, expected.length);

        const projectResults = coll.find({"a.b": {$eq: null}}, projectToOnlyADotB).toArray();
        assert(resultsEq(projectResults,
                         [
                             {a: {}},
                             {},
                             {},
                             {a: {b: null}},
                             {},
                             {},
                             {a: [{b: null}, {b: undefined}, {b: null}, {}]},
                             {a: [{b: null}, {b: 3}, {b: null}]},
                             {a: [{b: 3}, {}]},
                         ]),
               tojson(projectResults));
    }());

    // Test the semantics of the query {"a.b": {$ne: null}}.
    (function testDottedNotEqualsNull() {
        const noProjectResults = coll.find({"a.b": {$ne: null}}).toArray();
        const expected = [
            {_id: "a_subobject_b_not_null", a: {b: "hi"}},
            {_id: "a_subobject_b_undefined", a: {b: undefined}},
            {_id: "a_double_array", a: [[]]},
            {_id: "a_empty_array", a: []},
            {_id: "a_object_array_no_b_nulls", a: [{b: 1}, {b: 3}, {b: "string"}]},
            {_id: "a_object_array_some_b_undefined", a: [{b: undefined}, {b: 3}]},
            {_id: "a_value_array_all_nulls", a: [null, null]},
            {_id: "a_value_array_no_nulls", a: [1, "string", 4]},
            {_id: "a_value_array_with_null", a: [1, "string", null, 4]},
            {_id: "a_value_array_with_undefined", a: [1, "string", undefined, 4]}
        ];
        assert(resultsEq(noProjectResults, expected), tojson(noProjectResults));

        const count = coll.count({"a.b": {$ne: null}});
        assert.eq(count, expected.length);

        const projectResults = coll.find({"a.b": {$ne: null}}, projectToOnlyADotB).toArray();
        assert(resultsEq(projectResults,
                         [
                             {a: {b: "hi"}},
                             {a: {b: undefined}},
                             {a: [[]]},
                             {a: []},
                             {a: [{b: 1}, {b: 3}, {b: "string"}]},
                             {a: [{b: undefined}, {b: 3}]},
                             {a: []},
                             {a: []},
                             {a: []},
                             {a: []}
                         ]),
               tojson(projectResults));
    }());

    // Test the semantics of the query {a.b: {$in: [null, <number>]}}.
    (function testDottedInNullQuery() {
        const query = {"a.b": {$in: [null, 75]}};
        const noProjectResults = coll.find(query).toArray();
        const expected = [
            {_id: "a_empty_subobject", a: {}},
            {_id: "a_null", a: null},
            {_id: "a_number", a: 4},
            {_id: "a_subobject_b_null", a: {b: null}},
            {_id: "a_undefined", a: undefined},
            {_id: "no_a"},
            {
                _id: "a_object_array_b_mix_null_undefined_missing",
                a: [{b: null}, {b: undefined}, {b: null}, {}]
            },
            {_id: "a_object_array_some_b_nulls", a: [{b: null}, {b: 3}, {b: null}]},
            {_id: "a_object_array_some_b_missing", a: [{b: 3}, {}]},
        ];

        const count = coll.count({"a.b": {$in: [null, 75]}});
        assert.eq(count, expected.length);

        assert(resultsEq(noProjectResults, expected), noProjectResults);
    }());

    // Test the semantics of the query {$or: [{a.b: null}, {a.b: <number>}]}.
    (function testDottedOrNullQuery() {
        const query = {$or: [{"a.b": null}, {"a.b": 75}]};
        const noProjectResults = coll.find(query).toArray();
        const expected = [
            {_id: "a_empty_subobject", a: {}},
            {_id: "a_null", a: null},
            {_id: "a_number", a: 4},
            {_id: "a_subobject_b_null", a: {b: null}},
            {_id: "a_undefined", a: undefined},
            {_id: "no_a"},
            {
                _id: "a_object_array_b_mix_null_undefined_missing",
                a: [{b: null}, {b: undefined}, {b: null}, {}]
            },
            {_id: "a_object_array_some_b_nulls", a: [{b: null}, {b: 3}, {b: null}]},
            {_id: "a_object_array_some_b_missing", a: [{b: 3}, {}]},
        ];

        const count = coll.count({$or: [{"a.b": null}, {"a.b": 75}]});
        assert.eq(count, expected.length);

        assert(resultsEq(noProjectResults, expected), noProjectResults);
    }());

    // Test the semantics of the query {a.b: {$nin: [null, <number>]}}.
    (function testDottedNotInNullQuery() {
        const query = {"a.b": {$nin: [null, 75]}};
        const noProjectResults = coll.find(query).toArray();
        const expected = [
            {_id: "a_subobject_b_not_null", a: {b: "hi"}},
            {_id: "a_subobject_b_undefined", a: {b: undefined}},
            {_id: "a_double_array", a: [[]]},
            {_id: "a_empty_array", a: []},
            {_id: "a_object_array_no_b_nulls", a: [{b: 1}, {b: 3}, {b: "string"}]},
            {_id: "a_object_array_some_b_undefined", a: [{b: undefined}, {b: 3}]},
            {_id: "a_value_array_all_nulls", a: [null, null]},
            {_id: "a_value_array_no_nulls", a: [1, "string", 4]},
            {_id: "a_value_array_with_null", a: [1, "string", null, 4]},
            {_id: "a_value_array_with_undefined", a: [1, "string", undefined, 4]},
        ];

        assert(resultsEq(noProjectResults, expected), noProjectResults);

        const count = coll.count({"a.b": {$nin: [null, 75]}});
        assert.eq(count, expected.length);

        const projectResults = coll.find(query, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), projectResults);
    }());

    // Test the semantics of the query {a.b: {$nin: [null, <regex>]}}.
    (function testDottedNotInNullAndRegexQuery() {
        const query = {"a.b": {$nin: [null, /^str.*/]}};
        const noProjectResults = coll.find(query).toArray();
        const expected = [
            {_id: "a_subobject_b_not_null", a: {b: "hi"}},
            {_id: "a_subobject_b_undefined", a: {b: undefined}},
            {_id: "a_double_array", a: [[]]},
            {_id: "a_empty_array", a: []},
            {_id: "a_object_array_some_b_undefined", a: [{b: undefined}, {b: 3}]},
            {_id: "a_value_array_all_nulls", a: [null, null]},
            {_id: "a_value_array_no_nulls", a: [1, "string", 4]},
            {_id: "a_value_array_with_null", a: [1, "string", null, 4]},
            {_id: "a_value_array_with_undefined", a: [1, "string", undefined, 4]},
        ];

        const count = coll.count({"a.b": {$nin: [null, /^str.*/]}});
        assert.eq(count, expected.length);

        assert(resultsEq(noProjectResults, expected), noProjectResults);

        const projectResults = coll.find(query, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), projectResults);
    }());

    // Test the results of similar dotted queries with an $elemMatch. These should have no
    // results since none of our documents have an array at the path "a.b".
    (function testDottedElemMatchValue() {
        let results = coll.find({"a.b": {$elemMatch: {$eq: null}}}).toArray();
        assert(resultsEq(results, []), tojson(results));

        const count = coll.count({"a.b": {$elemMatch: {$eq: null}}});
        assert.eq(count, 0);

        results = coll.find({"a.b": {$elemMatch: {$ne: null}}}).toArray();
        assert(resultsEq(results, []), tojson(results));
    }());

    // Test null semantics within an $elemMatch object.
    (function testElemMatchObject() {
        // Test $elemMatch with equality to null.
        let noProjectResults = coll.find({a: {$elemMatch: {b: {$eq: null}}}}).toArray();
        const expectedEqualToNull = [
            {_id: "a_double_array", a: [[]]},
            {
                _id: "a_object_array_b_mix_null_undefined_missing",
                a: [{b: null}, {b: undefined}, {b: null}, {}]
            },
            {_id: "a_object_array_some_b_nulls", a: [{b: null}, {b: 3}, {b: null}]},
            {_id: "a_object_array_some_b_missing", a: [{b: 3}, {}]},
        ];
        assert(resultsEq(noProjectResults, expectedEqualToNull), tojson(noProjectResults));

        const count = coll.count({a: {$elemMatch: {b: {$eq: null}}}});
        assert.eq(count, expectedEqualToNull.length);

        let projectResults =
            coll.find({a: {$elemMatch: {b: {$eq: null}}}}, projectToOnlyADotB).toArray();
        assert(resultsEq(projectResults,
                         [
                             {a: [[]]},
                             {a: [{b: null}, {b: undefined}, {b: null}, {}]},
                             {a: [{b: null}, {b: 3}, {b: null}]},
                             {a: [{b: 3}, {}]},
                         ]),
               tojson(projectResults));

        // Test $elemMatch with not equal to null.
        noProjectResults = coll.find({a: {$elemMatch: {b: {$ne: null}}}}).toArray();
        const expectedNotEqualToNull = [
            {_id: "a_object_array_no_b_nulls", a: [{b: 1}, {b: 3}, {b: "string"}]},
            {_id: "a_object_array_some_b_nulls", a: [{b: null}, {b: 3}, {b: null}]},
            {_id: "a_object_array_some_b_undefined", a: [{b: undefined}, {b: 3}]},
            {_id: "a_object_array_some_b_missing", a: [{b: 3}, {}]},
            {
                _id: "a_object_array_b_mix_null_undefined_missing",
                a: [{b: null}, {b: undefined}, {b: null}, {}]
            },
        ];
        assert(resultsEq(noProjectResults, expectedNotEqualToNull), tojson(noProjectResults));

        projectResults =
            coll.find({a: {$elemMatch: {b: {$ne: null}}}}, projectToOnlyADotB).toArray();
        assert(resultsEq(projectResults,
                         [
                             {a: [{b: 1}, {b: 3}, {b: "string"}]},
                             {a: [{b: null}, {b: 3}, {b: null}]},
                             {a: [{b: undefined}, {b: 3}]},
                             {a: [{b: 3}, {}]},
                             {a: [{b: null}, {b: undefined}, {b: null}, {}]}
                         ]),
               tojson(projectResults));
    }());
}

// Test without any indexes.
jsTestLog('Without any indexes');
const collNamePrefix = 'null_query_semantics_';
let collCount = 0;
let coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
testNullSemantics(coll);

const keyPatterns = [
    {keyPattern: {a: 1}},
    {keyPattern: {a: -1}},
    {keyPattern: {a: "hashed"}},
    {keyPattern: {a: 1}, options: {partialFilterExpression: {a: {$exists: true}}}},
    {keyPattern: {a: 1}, options: {sparse: true}},
    {keyPattern: {"a.b": 1}},
    {keyPattern: {_id: 1, "a.b": 1}},
    {keyPattern: {"a.b": 1, _id: 1}},
    {keyPattern: {"a.b": 1}, options: {partialFilterExpression: {a: {$exists: true}}}},
    {keyPattern: {"a.b": 1, _id: 1}, options: {sparse: true}},
    {keyPattern: {"$**": 1}},
    {keyPattern: {"a.$**": 1}}
];

// Include Columnstore Index only if FF is enabled and collection is not clustered.
if (setUpServerForColumnStoreIndexTest(db)) {
    keyPatterns.push({keyPattern: {"$**": "columnstore"}});
}

// Test with a variety of other indexes.
for (let indexSpec of keyPatterns) {
    coll = db.getCollection(collNamePrefix + collCount++);
    coll.drop();
    jsTestLog(`Index spec: ${tojson(indexSpec)}`);
    assert.commandWorked(coll.createIndex(indexSpec.keyPattern, indexSpec.options));
    testNullSemantics(coll);
}

// Test that you cannot use a $ne: null predicate in a partial filter expression.
jsTestLog('Cannot use $ne: null predicate in a partial filter - 1');
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandFailedWithCode(coll.createIndex({a: 1}, {partialFilterExpression: {a: {$ne: null}}}),
                             ErrorCodes.CannotCreateIndex);

jsTestLog('Cannot use $ne: null predicate in a partial filter - 2');
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandFailedWithCode(
    coll.createIndex({a: 1}, {partialFilterExpression: {a: {$elemMatch: {$ne: null}}}}),
    ErrorCodes.CannotCreateIndex);
