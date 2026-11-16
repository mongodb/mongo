/**
 * Tests building indexes on edge-case documents.
 */

import {getBsonCorpus} from "jstests/libs/format_test_bson_corpus.js";
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";

let conn;
let coll;

function validate(expectedRecords, expectedKeys) {
    const result = assert.commandWorked(coll.validate());
    assert(result.valid, "Collection validation failed", {result});
    assert.eq(expectedRecords, result.nrecords, "Unexpected record count", {result});
    if (expectedKeys !== undefined) {
        assert.eq(expectedKeys, result.keysPerIndex.edge, "Unexpected index key count", {result});
    }
}

// One index at a time avoids interactions with earlier builds and the per-collection index limit.
function runCase({name, docs, indexes}) {
    describe(name, function () {
        before(function () {
            jsTest.log.info("BSON index-build case", {name});
            coll.drop();
            for (const doc of docs) {
                assert.commandWorked(coll.insert(doc), {name});
            }
        });

        afterEach(function () {
            assert.commandWorked(coll.dropIndexes(), {name});
        });

        for (const {key, options = {}, error, keys} of indexes) {
            const context = {name, key, options};
            it(`${tojson(key)} ${tojson(options)}`, function () {
                jsTest.log.info("Building index", context);
                const result = coll.createIndex(key, {...options, name: "edge"});
                if (error !== undefined) {
                    assert.commandFailedWithCode(result, error, context);
                    assert.eq(
                        ["_id_"],
                        coll.getIndexes().map((index) => index.name),
                        "Failed build left an index behind",
                        context,
                    );
                    validate(docs.length);
                } else {
                    assert.commandWorked(result, context);
                    const index = coll.getIndexes().find((index) => index.name === "edge");
                    assert.neq(undefined, index, "Built index is missing", context);
                    assert.eq(key, index.key, "Unexpected index key pattern", context);
                    for (const option of ["unique", "sparse"]) {
                        assert.eq(!!options[option], !!index[option], "Unexpected index option", {
                            ...context,
                            option,
                            index,
                        });
                    }
                    validate(docs.length, keys);
                }
            });
        }
    });
}

describe("Index builds on edge-case documents", function () {
    before(function () {
        conn = MongoRunner.runMongod({});
        coll = conn.getDB(jsTestName()).corpus;
    });

    after(function () {
        if (conn) {
            MongoRunner.stopMongod(conn);
        }
    });

    const corpus = getBsonCorpus();

    // For large nested documents, select one whole subobject and one scalar leaf path instead of
    // building a separate index on every field. The wildcard build below covers all indexable fields.
    const selectedPaths = {
        wide_tree: ["1a", "1a.".repeat(8) + "foo"],
        deep_nesting: ["1a", "1a.".repeat(146) + "foo"],
        packed_subobjects: ["100000000a", "100000000a." + "1a.".repeat(6) + "foo"],
    };
    const arraySubpaths = {
        containers: ["docArr.a", "mixedArr.four"],
    };
    for (const {name, doc} of corpus) {
        const paths = selectedPaths[name] ?? Object.keys(doc).filter((path) => path !== "_id");
        if (name === "field_missing") {
            paths.push("maybe");
        }
        // Share one insertion across all paths and index types for this document.
        const indexes = [];
        for (const path of paths) {
            const array = Array.isArray(doc[path]);
            const keyCount = name === "giant_array" ? {keys: 0x100000} : {};
            // Other arrays cover ascending builds; avoid a redundant million-key build here.
            const directions = name === "giant_array" ? [-1] : [1, -1];
            indexes.push(
                ...directions.map((direction) => ({key: {[path]: direction}, ...keyCount})),
                {key: {[path]: "hashed"}, ...(array ? {error: 16766} : {})},
            );
        }
        indexes.push({key: {"$**": 1}});
        for (const path of arraySubpaths[name] ?? []) {
            indexes.push(
                {key: {[path]: 1}},
                {key: {[path]: -1}},
                {key: {[path]: 1, _id: -1}, options: {unique: true}},
                {key: {[path]: "hashed"}, error: 16766},
            );
        }
        for (const path of new Set((arraySubpaths[name] ?? []).map((path) => path.split(".")[0]))) {
            indexes.push({key: {[`${path}.$**`]: 1}}, {key: {_id: 1, [`${path}.$**`]: 1}});
        }
        runCase({name, docs: [doc], indexes});
    }

    // Supplement the corpus with combinations of unusual values.
    const numericGroups = [
        [NumberInt(1), NumberLong(1), 1.0, NumberDecimal("1.0"), NumberDecimal("1.00")],
        [0.0, -0.0, NumberDecimal("0"), NumberDecimal("-0"), NumberDecimal("0E+10")],
        [NaN, NumberDecimal("+NaN"), NumberDecimal("-NaN")],
        [Infinity, NumberDecimal("+Infinity")],
        [-Infinity, NumberDecimal("-Infinity")],
    ];
    for (let i = 0; i < numericGroups.length; ++i) {
        const values = numericGroups[i];
        runCase({
            name: `numeric_equivalence_${i}`,
            docs: values.map((a, j) => ({_id: j, a})),
            indexes: [
                {key: {a: 1}},
                {key: {a: -1}},
                {key: {a: "hashed"}},
                {key: {a: "hashed", _id: -1}, keys: values.length},
                {key: {a: 1}, options: {unique: true}, error: ErrorCodes.DuplicateKey},
                {key: {a: 1, _id: -1}, options: {unique: true}, keys: values.length},
            ],
        });
    }

    runCase({
        name: "adjacent_int64_values",
        docs: [
            {_id: 0, a: NumberLong("9007199254740992")},
            {_id: 1, a: NumberLong("9007199254740993")},
        ],
        indexes: [
            {key: {a: 1}, options: {unique: true}, keys: 2},
            {key: {a: -1}},
            {key: {a: "hashed"}},
        ],
    });

    const longField = "long_field_" + "x".repeat(1024);
    runCase({
        name: "empty_and_long_field_names",
        docs: [{_id: 0, "": 1, object: {"": 2}, [longField]: "value"}],
        indexes: [
            {key: {[longField]: 1}, keys: 1},
            {key: {[longField]: -1}, keys: 1},
            {key: {[longField]: "hashed"}, keys: 1},
            {key: {object: 1}, keys: 1},
        ],
    });

    runCase({
        name: "large_compound_multikey",
        docs: [
            {
                _id: 0,
                [longField]: "x".repeat(256 * 1024),
                b: ["y".repeat(256 * 1024), "z".repeat(256 * 1024)],
            },
            {_id: 1, [longField]: "x".repeat(256 * 1024), b: ["w".repeat(256 * 1024)]},
        ],
        indexes: [
            {key: {[longField]: 1, b: -1}, keys: 3},
            {key: {[longField]: -1, b: 1}, options: {unique: true}, keys: 3},
            {key: {[longField]: "hashed", b: 1}, error: 16766},
        ],
    });

    runCase({
        name: "correlated_compound_multikey_missing_and_empty",
        docs: [
            {_id: 0, a: [{b: 1}, {c: 2}, {}]},
            {
                _id: 1,
                a: [
                    {b: [], c: 3},
                    {b: {}, c: []},
                ],
            },
            {_id: 2, a: []},
        ],
        indexes: [
            {key: {"a.b": 1, "a.c": -1}, keys: 6},
            {key: {"a.c": -1, "a.b": 1}, keys: 6},
        ],
    });
});
