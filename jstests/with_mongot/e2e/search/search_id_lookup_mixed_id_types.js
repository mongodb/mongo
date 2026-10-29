/**
 * End-to-end $search idLookup for mixed scalar and object `_id`s (SERVER-134017).
 *
 * With featureFlagSearchOptimizedIdLookup, SBE encodes scalar `_id`s (number, string, OID, …)
 * and returns kNotHandled for object `_id`s. Local-read fallback must still return those
 * documents (AF-20648). Array `_id`s are omitted because MongoDB rejects them on insert.
 *
 * Lives under e2e/search so it runs against real mongot and the search-extension suites.
 *
 * @tags: [requires_getmore]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";

const oid = ObjectId("507f1f77bcf86cd799439011");
const date = ISODate("2020-01-02T03:04:05Z");
const uuid = UUID("81fd5473-1747-4c9d-8743-f10642b3bb99");
const ts = Timestamp(1, 1);
const decimal = NumberDecimal("1.5");

const indexName = jsTestName() + "_index";
const searchStage = {$search: {index: indexName, text: {query: "lookup", path: "title"}}};

const objectDocs = [
    {_id: {a: 1}, title: "lookup object"},
    {_id: {a: 1, b: 2}, title: "lookup compound"},
    {_id: {nested: {x: 1}}, title: "lookup nested"},
];

const mixedDocs = [
    {_id: 42, title: "lookup number"},
    {_id: "str", title: "lookup string"},
    {_id: oid, title: "lookup oid"},
    {_id: date, title: "lookup date"},
    {_id: true, title: "lookup bool"},
    {_id: BinData(0, "AQID"), title: "lookup bindata"},
    {_id: NumberLong(99), title: "lookup long"},
    {_id: decimal, title: "lookup decimal"},
    {_id: ts, title: "lookup timestamp"},
    {_id: uuid, title: "lookup uuid"},
    {_id: {a: 1, b: 2}, title: "lookup object"},
    {_id: {nested: {x: 1}}, title: "lookup nested"},
];

function createIndex(coll) {
    createSearchIndex(coll, {
        name: indexName,
        definition: {mappings: {dynamic: false, fields: {title: {type: "string"}}}},
    });
}

function drainParkedSearch(coll, expected) {
    const res = assert.commandWorked(
        db.runCommand({
            aggregate: coll.getName(),
            pipeline: [searchStage],
            cursor: {batchSize: 1},
        }),
    );
    assert.eq(1, res.cursor.firstBatch.length, "expected a parked cursor after the first batch", {
        res,
    });
    assert.neq(NumberLong(0), res.cursor.id, "expected a non-zero cursor id", {res});

    const actual = [...res.cursor.firstBatch];
    let cursorId = res.cursor.id;
    while (cursorId != 0) {
        const more = assert.commandWorked(
            db.runCommand({getMore: cursorId, collection: coll.getName(), batchSize: 1}),
        );
        actual.push(...more.cursor.nextBatch);
        cursorId = more.cursor.id;
    }
    assertArrayEq({actual, expected});
}

describe("$search idLookup with object `_id`s", function () {
    const coll = db.getCollection(jsTestName() + "_object");

    before(function () {
        coll.drop();
        assert.commandWorked(coll.insertMany(objectDocs));
        createIndex(coll);
    });

    after(function () {
        dropSearchIndex(coll, {name: indexName});
        coll.drop();
    });

    it("returns documents whose `_id`s are objects", function () {
        assertArrayEq({actual: coll.aggregate([searchStage]).toArray(), expected: objectDocs});
    });

    it("returns the same object `_id`s across getMore batches", function () {
        drainParkedSearch(coll, objectDocs);
    });
});

describe("$search idLookup with mixed scalar and object `_id`s", function () {
    const coll = db.getCollection(jsTestName() + "_mixed");

    before(function () {
        coll.drop();
        assert.commandWorked(coll.insertMany(mixedDocs));
        createIndex(coll);
    });

    after(function () {
        dropSearchIndex(coll, {name: indexName});
        coll.drop();
    });

    it("returns documents with mixed scalar and object `_id`s", function () {
        assertArrayEq({actual: coll.aggregate([searchStage]).toArray(), expected: mixedDocs});
    });

    it("returns the same mixed `_id`s across getMore batches", function () {
        drainParkedSearch(coll, mixedDocs);
    });
});
