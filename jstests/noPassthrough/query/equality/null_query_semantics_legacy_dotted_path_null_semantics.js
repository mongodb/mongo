// Tests the pre-fix behavior of null queries on dotted paths when
// internalQueryLegacyDottedPathNullSemantics is set to true (SERVER-36681).
// With the fix disabled, empty arrays, scalar-only arrays, and arrays containing only nested
// empty arrays do NOT match a null predicate on a dotted path.
// @tags: [
//   requires_fcv_80,
//   requires_getmore,
// ]
//
import {resultsEq} from "jstests/aggregation/extras/utils.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

const origDisableFix = assert.commandWorked(
    db.adminCommand({getParameter: 1, internalQueryLegacyDottedPathNullSemantics: 1}),
).internalQueryLegacyDottedPathNullSemantics;
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryLegacyDottedPathNullSemantics: true}));

function extractAValues(results) {
    return results.map(function (res) {
        if (!res.hasOwnProperty("a")) {
            return {};
        }
        return {a: res.a};
    });
}

function testNullSemantics(coll) {
    assert.commandWorked(
        coll.insert([
            {_id: "a_empty_subobject", a: {}},
            {_id: "a_null", a: null},
            {_id: "a_number", a: 4},
            {_id: "a_subobject_b_not_null", a: {b: "hi"}},
            {_id: "a_subobject_b_null", a: {b: null}},
            {_id: "a_subobject_b_undefined", a: {b: undefined}},
            {_id: "a_undefined", a: undefined},
            {_id: "no_a"},
        ]),
    );

    const projectToOnlyA = {_id: 0, a: 1};
    const projectToOnlyADotB = {_id: 0, "a.b": 1};

    (function testBasicNullQuery() {
        const noProjectResults = coll.find({a: {$eq: null}}).toArray();
        const expected = [{_id: "a_null", a: null}, {_id: "no_a"}];
        assert(resultsEq(expected, noProjectResults), tojson(noProjectResults));

        const count = coll.count({a: {$eq: null}});
        assert.eq(count, expected.length);

        const projectResults = coll.find({a: {$eq: null}}, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), tojson(projectResults));
    })();

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
    })();

    (function testInNullQuery() {
        const query = {a: {$in: [null, 4]}};
        const noProjectResults = coll.find(query).toArray();
        const expected = [{_id: "a_null", a: null}, {_id: "a_number", a: 4}, {_id: "no_a"}];

        const count = coll.count(query);
        assert.eq(count, expected.length);

        assert(resultsEq(noProjectResults, expected), noProjectResults);

        const projectResults = coll.find(query, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), projectResults);
    })();

    (function testOrNullQuery() {
        const query = {$or: [{a: null}, {a: 4}]};
        const noProjectResults = coll.find(query).toArray();
        const expected = [{_id: "a_null", a: null}, {_id: "a_number", a: 4}, {_id: "no_a"}];

        const count = coll.count(query);
        assert.eq(count, expected.length);

        assert(resultsEq(noProjectResults, expected), noProjectResults);

        const projectResults = coll.find(query, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), projectResults);
    })();

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
    })();

    (function testNotInNullAndRegexQuery() {
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
    })();

    (function testExistsFalse() {
        const noProjectResults = coll.find({a: {$exists: false}}).toArray();
        const expected = [{_id: "no_a"}];
        assert(resultsEq(noProjectResults, expected), tojson(noProjectResults));

        const count = coll.count({a: {$exists: false}});
        assert.eq(count, expected.length);

        const projectResults = coll.find({a: {$exists: false}}, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), tojson(projectResults));
    })();

    // Documents without array values for a are unaffected by the fix.
    (function testDottedEqualsNull() {
        const noProjectResults = coll.find({"a.b": {$eq: null}}).toArray();
        const expected = [
            {_id: "a_empty_subobject", a: {}},
            {_id: "a_null", a: null},
            {_id: "a_number", a: 4},
            {_id: "a_subobject_b_null", a: {b: null}},
            {_id: "a_undefined", a: undefined},
            {_id: "no_a"},
        ];
        assert(resultsEq(noProjectResults, expected), tojson(noProjectResults));

        const count = coll.count({"a.b": {$eq: null}});
        assert.eq(count, expected.length);

        const projectResults = coll.find({"a.b": {$eq: null}}, projectToOnlyADotB).toArray();
        assert(resultsEq(projectResults, [{a: {}}, {}, {}, {a: {b: null}}, {}, {}]), tojson(projectResults));
    })();

    (function testDottedNotEqualsNull() {
        const noProjectResults = coll.find({"a.b": {$ne: null}}).toArray();
        const expected = [
            {_id: "a_subobject_b_not_null", a: {b: "hi"}},
            {_id: "a_subobject_b_undefined", a: {b: undefined}},
        ];
        assert(resultsEq(noProjectResults, expected), tojson(noProjectResults));

        const count = coll.count({"a.b": {$ne: null}});
        assert.eq(count, expected.length);

        const projectResults = coll.find({"a.b": {$ne: null}}, projectToOnlyADotB).toArray();
        assert(resultsEq(projectResults, [{a: {b: "hi"}}, {a: {b: undefined}}]), tojson(projectResults));
    })();

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
    })();

    (function testElemMatchQueriesWithNoArrays() {
        for (let elemMatchQuery of [
            {a: {$elemMatch: {$eq: null}}},
            {a: {$elemMatch: {$ne: null}}},
            {"a.b": {$elemMatch: {$eq: null}}},
            {"a.b": {$elemMatch: {$ne: null}}},
            {a: {$elemMatch: {b: {$eq: null}}}},
            {a: {$elemMatch: {b: {$ne: null}}}},
        ]) {
            const noProjectResults = coll.find(elemMatchQuery).toArray();
            assert(
                resultsEq(noProjectResults, []),
                `Expected no results for query ${tojson(elemMatchQuery)}, got ` + tojson(noProjectResults),
            );

            const count = coll.count(elemMatchQuery);
            assert.eq(count, 0);

            let projectResults = coll.find(elemMatchQuery, projectToOnlyA).toArray();
            assert(
                resultsEq(projectResults, []),
                `Expected no results for query ${tojson(elemMatchQuery)}, got ` + tojson(projectResults),
            );

            projectResults = coll.find(elemMatchQuery, projectToOnlyADotB).toArray();
            assert(
                resultsEq(projectResults, []),
                `Expected no results for query ${tojson(elemMatchQuery)}, got ` + tojson(projectResults),
            );
        }
    })();

    const writeResult = coll.insert([
        {_id: "a_double_array", a: [[]]},
        {_id: "a_empty_array", a: []},
        {
            _id: "a_object_array_b_mix_null_undefined_missing",
            a: [{b: null}, {b: undefined}, {b: null}, {}],
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
        assert.eq(writeResult.getWriteErrors().length, 1, tojson(writeResult));
        assert.eq(writeResult.getWriteErrors()[0].code, 16766, tojson(writeResult));
        return;
    }
    assert.commandWorked(writeResult);

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
    })();

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
                a: [{b: null}, {b: undefined}, {b: null}, {}],
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
    })();

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
                a: [{b: null}, {b: undefined}, {b: null}, {}],
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
    })();

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
    })();

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
    })();

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
                a: [{b: null}, {b: undefined}, {b: null}, {}],
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
    })();

    (function testNotInNullAndEmptyArrayQuery() {
        const query = {a: {$nin: [null, []]}};
        const noProjectResults = coll.find(query).toArray();
        const expected = [
            {_id: "a_empty_subobject", a: {}},
            {_id: "a_number", a: 4},
            {_id: "a_subobject_b_not_null", a: {b: "hi"}},
            {_id: "a_subobject_b_null", a: {b: null}},
            {_id: "a_subobject_b_undefined", a: {b: undefined}},
            {_id: "a_undefined", a: undefined},

            {
                _id: "a_object_array_b_mix_null_undefined_missing",
                a: [{b: null}, {b: undefined}, {b: null}, {}],
            },
            {_id: "a_object_array_no_b_nulls", a: [{b: 1}, {b: 3}, {b: "string"}]},
            {_id: "a_object_array_some_b_nulls", a: [{b: null}, {b: 3}, {b: null}]},
            {_id: "a_object_array_some_b_undefined", a: [{b: undefined}, {b: 3}]},
            {_id: "a_object_array_some_b_missing", a: [{b: 3}, {}]},
            {_id: "a_value_array_no_nulls", a: [1, "string", 4]},
            {_id: "a_value_array_with_undefined", a: [1, "string", undefined, 4]},
        ];

        assert(resultsEq(noProjectResults, expected), noProjectResults);

        const count = coll.count({a: {$nin: [null, []]}});
        assert.eq(count, expected.length);

        const projectResults = coll.find(query, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expected)), projectResults);
    })();

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
                a: [{b: null}, {b: undefined}, {b: null}, {}],
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
    })();

    (function testElemMatchValue() {
        let noProjectResults = coll.find({a: {$elemMatch: {$eq: null}}}).toArray();
        const expectedEqualToNull = [
            {_id: "a_value_array_all_nulls", a: [null, null]},
            {_id: "a_value_array_with_null", a: [1, "string", null, 4]},
        ];
        assert(resultsEq(noProjectResults, expectedEqualToNull), tojson(noProjectResults));

        const count = coll.count({a: {$elemMatch: {$eq: null}}});
        assert.eq(count, expectedEqualToNull.length);

        let projectResults = coll.find({a: {$elemMatch: {$eq: null}}}, projectToOnlyA).toArray();
        assert(resultsEq(projectResults, extractAValues(expectedEqualToNull)), tojson(projectResults));

        noProjectResults = coll.find({a: {$elemMatch: {$ne: null}}}).toArray();
        const expectedNotEqualToNull = [
            {_id: "a_double_array", a: [[]]},
            {
                _id: "a_object_array_b_mix_null_undefined_missing",
                a: [{b: null}, {b: undefined}, {b: null}, {}],
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
        assert(resultsEq(projectResults, extractAValues(expectedNotEqualToNull)), tojson(projectResults));
    })();

    // With fix disabled, empty arrays, scalar-only arrays, and nested-empty-array arrays do NOT
    // match {"a.b": null}. They instead fall through to match {"a.b": {$ne: null}}.
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
                a: [{b: null}, {b: undefined}, {b: null}, {}],
            },
            {_id: "a_object_array_some_b_nulls", a: [{b: null}, {b: 3}, {b: null}]},
            {_id: "a_object_array_some_b_missing", a: [{b: 3}, {}]},
        ];
        assert(resultsEq(noProjectResults, expected), tojson(noProjectResults));

        const count = coll.count({"a.b": {$eq: null}});
        assert.eq(count, expected.length);

        const projectResults = coll.find({"a.b": {$eq: null}}, projectToOnlyADotB).toArray();
        assert(
            resultsEq(projectResults, [
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
            tojson(projectResults),
        );
    })();

    (function testDottedNotEqualsNull() {
        const noProjectResults = coll.find({"a.b": {$ne: null}}).toArray();
        const expected = [
            {_id: "a_subobject_b_not_null", a: {b: "hi"}},
            {_id: "a_subobject_b_undefined", a: {b: undefined}},
            {_id: "a_object_array_no_b_nulls", a: [{b: 1}, {b: 3}, {b: "string"}]},
            {_id: "a_object_array_some_b_undefined", a: [{b: undefined}, {b: 3}]},
            // With fix disabled, these no longer match $eq: null so they match $ne: null.
            {_id: "a_double_array", a: [[]]},
            {_id: "a_empty_array", a: []},
            {_id: "a_value_array_all_nulls", a: [null, null]},
            {_id: "a_value_array_no_nulls", a: [1, "string", 4]},
            {_id: "a_value_array_with_null", a: [1, "string", null, 4]},
            {_id: "a_value_array_with_undefined", a: [1, "string", undefined, 4]},
        ];
        assert(resultsEq(noProjectResults, expected), tojson(noProjectResults));

        const count = coll.count({"a.b": {$ne: null}});
        assert.eq(count, expected.length);

        const projectResults = coll.find({"a.b": {$ne: null}}, projectToOnlyADotB).toArray();
        assert(
            resultsEq(projectResults, [
                {a: {b: "hi"}},
                {a: {b: undefined}},
                {a: [{b: 1}, {b: 3}, {b: "string"}]},
                {a: [{b: undefined}, {b: 3}]},
                {a: [[]]},
                {a: []},
                {a: []},
                {a: []},
                {a: []},
                {a: []},
            ]),
            tojson(projectResults),
        );
    })();

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
                a: [{b: null}, {b: undefined}, {b: null}, {}],
            },
            {_id: "a_object_array_some_b_nulls", a: [{b: null}, {b: 3}, {b: null}]},
            {_id: "a_object_array_some_b_missing", a: [{b: 3}, {}]},
        ];

        const count = coll.count({"a.b": {$in: [null, 75]}});
        assert.eq(count, expected.length);

        assert(resultsEq(noProjectResults, expected), noProjectResults);
    })();

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
                a: [{b: null}, {b: undefined}, {b: null}, {}],
            },
            {_id: "a_object_array_some_b_nulls", a: [{b: null}, {b: 3}, {b: null}]},
            {_id: "a_object_array_some_b_missing", a: [{b: 3}, {}]},
        ];

        const count = coll.count({$or: [{"a.b": null}, {"a.b": 75}]});
        assert.eq(count, expected.length);

        assert(resultsEq(noProjectResults, expected), noProjectResults);
    })();

    (function testDottedNotInNullQuery() {
        const query = {"a.b": {$nin: [null, 75]}};
        const noProjectResults = coll.find(query).toArray();
        const expected = [
            {_id: "a_subobject_b_not_null", a: {b: "hi"}},
            {_id: "a_subobject_b_undefined", a: {b: undefined}},
            {_id: "a_object_array_no_b_nulls", a: [{b: 1}, {b: 3}, {b: "string"}]},
            {_id: "a_object_array_some_b_undefined", a: [{b: undefined}, {b: 3}]},
            // With fix disabled, these no longer match $eq: null so they match $nin: [null].
            {_id: "a_double_array", a: [[]]},
            {_id: "a_empty_array", a: []},
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
    })();

    (function testDottedNotInNullAndRegexQuery() {
        const query = {"a.b": {$nin: [null, /^str.*/]}};
        const noProjectResults = coll.find(query).toArray();
        const expected = [
            {_id: "a_subobject_b_not_null", a: {b: "hi"}},
            {_id: "a_subobject_b_undefined", a: {b: undefined}},
            {_id: "a_object_array_some_b_undefined", a: [{b: undefined}, {b: 3}]},
            // With fix disabled, empty/scalar/nested-empty-array arrays no longer match
            // $eq: null so they also pass $nin: [null, regex].
            {_id: "a_double_array", a: [[]]},
            {_id: "a_empty_array", a: []},
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
    })();

    (function testDottedElemMatchValue() {
        let results = coll.find({"a.b": {$elemMatch: {$eq: null}}}).toArray();
        assert(resultsEq(results, []), tojson(results));

        const count = coll.count({"a.b": {$elemMatch: {$eq: null}}});
        assert.eq(count, 0);

        results = coll.find({"a.b": {$elemMatch: {$ne: null}}}).toArray();
        assert(resultsEq(results, []), tojson(results));
    })();

    (function testElemMatchObject() {
        let noProjectResults = coll.find({a: {$elemMatch: {b: {$eq: null}}}}).toArray();
        const expectedEqualToNull = [
            {_id: "a_double_array", a: [[]]},
            {
                _id: "a_object_array_b_mix_null_undefined_missing",
                a: [{b: null}, {b: undefined}, {b: null}, {}],
            },
            {_id: "a_object_array_some_b_nulls", a: [{b: null}, {b: 3}, {b: null}]},
            {_id: "a_object_array_some_b_missing", a: [{b: 3}, {}]},
        ];
        assert(resultsEq(noProjectResults, expectedEqualToNull), tojson(noProjectResults));

        const count = coll.count({a: {$elemMatch: {b: {$eq: null}}}});
        assert.eq(count, expectedEqualToNull.length);

        let projectResults = coll.find({a: {$elemMatch: {b: {$eq: null}}}}, projectToOnlyADotB).toArray();
        assert(
            resultsEq(projectResults, [
                {a: [[]]},
                {a: [{b: null}, {b: undefined}, {b: null}, {}]},
                {a: [{b: null}, {b: 3}, {b: null}]},
                {a: [{b: 3}, {}]},
            ]),
            tojson(projectResults),
        );

        noProjectResults = coll.find({a: {$elemMatch: {b: {$ne: null}}}}).toArray();
        const expectedNotEqualToNull = [
            {_id: "a_object_array_no_b_nulls", a: [{b: 1}, {b: 3}, {b: "string"}]},
            {_id: "a_object_array_some_b_nulls", a: [{b: null}, {b: 3}, {b: null}]},
            {_id: "a_object_array_some_b_undefined", a: [{b: undefined}, {b: 3}]},
            {_id: "a_object_array_some_b_missing", a: [{b: 3}, {}]},
            {
                _id: "a_object_array_b_mix_null_undefined_missing",
                a: [{b: null}, {b: undefined}, {b: null}, {}],
            },
        ];
        assert(resultsEq(noProjectResults, expectedNotEqualToNull), tojson(noProjectResults));

        projectResults = coll.find({a: {$elemMatch: {b: {$ne: null}}}}, projectToOnlyADotB).toArray();
        assert(
            resultsEq(projectResults, [
                {a: [{b: 1}, {b: 3}, {b: "string"}]},
                {a: [{b: null}, {b: 3}, {b: null}]},
                {a: [{b: undefined}, {b: 3}]},
                {a: [{b: 3}, {}]},
                {a: [{b: null}, {b: undefined}, {b: null}, {}]},
            ]),
            tojson(projectResults),
        );
    })();
}

jsTestLog("Without any indexes");
const collNamePrefix = "null_query_semantics_disable_fix_";
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
    {keyPattern: {"a.$**": 1}},
];

for (let indexSpec of keyPatterns) {
    coll = db.getCollection(collNamePrefix + collCount++);
    coll.drop();
    jsTestLog(`Index spec: ${tojson(indexSpec)}`);
    assert.commandWorked(coll.createIndex(indexSpec.keyPattern, indexSpec.options));
    testNullSemantics(coll);
}

jsTestLog("Cannot use $ne: null predicate in a partial filter - 1");
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandFailedWithCode(
    coll.createIndex({a: 1}, {partialFilterExpression: {a: {$ne: null}}}),
    ErrorCodes.CannotCreateIndex,
);

jsTestLog("Cannot use $ne: null predicate in a partial filter - 2");
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandFailedWithCode(
    coll.createIndex({a: 1}, {partialFilterExpression: {a: {$elemMatch: {$ne: null}}}}),
    ErrorCodes.CannotCreateIndex,
);

assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryLegacyDottedPathNullSemantics: origDisableFix}));
MongoRunner.stopMongod(conn);
