/**
 * Correctness test for the SequentialDocumentCache BSON-materialization + $match whole-document fast
 * path (see sequential_document_cache.cpp and match_processor.cpp). A classic pipeline $lookup with
 * a cacheable uncorrelated prefix materializes each cached foreign document to owned BSON once when
 * the cache is frozen, and the correlated $match then matches against the whole (already-BSON-backed)
 * document instead of rebuilding a projected BSON on every input. Both are meant to be strictly
 * behavior-preserving.
 *
 * Since the optimization is unconditional (no toggle), each half is validated against a path that
 * bypasses it:
 *   - Materialization: comparing the default run against one with the $lookup cache disabled
 *     (internalDocumentSourceLookupCacheSizeBytes = 0), which re-executes the foreign sub-pipeline
 *     per input and never materializes. The query is identical, so all $expr semantics (null/missing,
 *     arrays, collation) are exercised without a hand-written oracle.
 *   - Whole-document match: comparing small foreign documents (which take the fast path) against the
 *     same documents padded beyond the 16KB size gate (which fall back to the projection path).
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("$lookup cached-prefix BSON materialization", function () {
    let conn;
    let testDB;
    let localColl;
    let foreignColl;
    let foreignName;

    const kDefaultCacheSizeBytes = 100 * 1024 * 1024;

    before(function () {
        conn = MongoRunner.runMongod({});
        assert.neq(null, conn, "mongod failed to start");
        testDB = conn.getDB("test");
        localColl = testDB.local;
        foreignColl = testDB.foreign;
        foreignName = foreignColl.getName();
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    function setLookupCacheSize(bytes) {
        assert.commandWorked(
            testDB.adminCommand({
                setParameter: 1,
                internalDocumentSourceLookupCacheSizeBytes: bytes,
            }),
        );
    }

    function reset(coll, docs) {
        coll.drop();
        if (docs.length > 0) {
            assert.commandWorked(coll.insert(docs));
        }
    }

    // Sort each result's 'match' array so the comparison is insensitive to intra-array ordering
    // (assertArrayEq is already order-independent for the outer array).
    function normalize(results) {
        return results.map((doc) => {
            const copy = Object.assign({}, doc);
            if (Array.isArray(copy.match)) {
                copy.match = [...copy.match].sort((a, b) => bsonWoCompare(a, b));
            }
            return copy;
        });
    }

    // Runs 'pipeline' with the cache enabled (materialization + whole-document match) and with the
    // cache disabled (no materialization), and asserts the results are identical.
    function assertMaterializationPreservesResults(pipeline, aggOpts, desc) {
        setLookupCacheSize(kDefaultCacheSizeBytes);
        const optimized = normalize(localColl.aggregate(pipeline, aggOpts).toArray());
        setLookupCacheSize(0);
        try {
            const reference = normalize(localColl.aggregate(pipeline, aggOpts).toArray());
            assertArrayEq({
                actual: optimized,
                expected: reference,
                extraErrorMsg: `cache materialization changed results for: ${desc}`,
            });
        } finally {
            setLookupCacheSize(kDefaultCacheSizeBytes);
        }
    }

    it("preserves results for the uncorrelated-prefix many-to-many join", function () {
        // The Aggregation.Lookup.UncorrelatedPrefixJoin benchmark shape: a cached $addFields prefix
        // followed by a correlated equality $match, joining on (_id % 5). With the cache disabled the
        // $addFields documents are unmaterialized (projection path), so this compares the fully
        // optimized path against the original one end-to-end.
        reset(
            localColl,
            Array.from({length: 25}, (_, i) => ({_id: i})),
        );
        reset(
            foreignColl,
            Array.from({length: 25}, (_, i) => ({_id: i, pad: "padding"})),
        );
        assertMaterializationPreservesResults(
            [
                {
                    $lookup: {
                        from: foreignName,
                        let: {foreignKey: "$_id"},
                        pipeline: [
                            {$addFields: {newField: {$mod: ["$_id", 5]}}},
                            {$match: {$expr: {$eq: ["$newField", {$mod: ["$$foreignKey", 5]}]}}},
                        ],
                        as: "match",
                    },
                },
            ],
            {},
            "uncorrelated-prefix many-to-many",
        );
    });

    it("preserves results for a simple equality with one-to-many and non-matching inputs", function () {
        reset(localColl, [
            {_id: 1, k: "a"},
            {_id: 2, k: "b"},
            {_id: 3, k: "no-match"},
        ]);
        reset(foreignColl, [
            {_id: 10, fk: "a"},
            {_id: 11, fk: "a"},
            {_id: 12, fk: "b"},
        ]);
        assertMaterializationPreservesResults(
            [
                {
                    $lookup: {
                        from: foreignName,
                        let: {lk: "$k"},
                        pipeline: [{$match: {$expr: {$eq: ["$fk", "$$lk"]}}}],
                        as: "match",
                    },
                },
            ],
            {},
            "simple equality, one-to-many + no-match",
        );
    });

    it("preserves results for null and missing keys", function () {
        reset(localColl, [{_id: 1, k: null}, {_id: 2 /* k missing */}, {_id: 3, k: 5}]);
        reset(foreignColl, [{_id: 10, fk: null}, {_id: 11 /* fk missing */}, {_id: 12, fk: 5}]);
        assertMaterializationPreservesResults(
            [
                {
                    $lookup: {
                        from: foreignName,
                        let: {lk: "$k"},
                        // The $addFields makes the cached documents modified, so they are materialized.
                        pipeline: [
                            {$addFields: {kept: "$fk"}},
                            {$match: {$expr: {$eq: ["$fk", "$$lk"]}}},
                        ],
                        as: "match",
                    },
                },
            ],
            {},
            "null/missing keys",
        );
    });

    it("preserves results for array-valued keys", function () {
        reset(localColl, [
            {_id: 1, k: [1, 2]},
            {_id: 2, k: 3},
        ]);
        reset(foreignColl, [
            {_id: 10, fk: [1, 2]},
            {_id: 11, fk: 3},
            {_id: 12, fk: [1, 2, 3]},
        ]);
        assertMaterializationPreservesResults(
            [
                {
                    $lookup: {
                        from: foreignName,
                        let: {lk: "$k"},
                        pipeline: [
                            {$addFields: {kept: "$fk"}},
                            {$match: {$expr: {$eq: ["$fk", "$$lk"]}}},
                        ],
                        as: "match",
                    },
                },
            ],
            {},
            "array-valued keys",
        );
    });

    it("preserves results for a dotted foreign key", function () {
        reset(localColl, [
            {_id: 1, k: "x"},
            {_id: 2, k: "y"},
        ]);
        reset(foreignColl, [
            {_id: 10, s: {v: "x"}},
            {_id: 11, s: {v: "y"}},
            {_id: 12, s: {v: "z"}},
        ]);
        assertMaterializationPreservesResults(
            [
                {
                    $lookup: {
                        from: foreignName,
                        let: {lk: "$k"},
                        pipeline: [
                            {$addFields: {flat: "$s.v"}},
                            {$match: {$expr: {$eq: ["$s.v", "$$lk"]}}},
                        ],
                        as: "match",
                    },
                },
            ],
            {},
            "dotted foreign key",
        );
    });

    it("preserves results under a case-insensitive collation", function () {
        // The materialized/whole-document match must respect the aggregation's collation.
        reset(localColl, [
            {_id: 1, k: "ABC"},
            {_id: 2, k: "def"},
        ]);
        reset(foreignColl, [
            {_id: 10, fk: "abc"},
            {_id: 11, fk: "DEF"},
            {_id: 12, fk: "xyz"},
        ]);
        assertMaterializationPreservesResults(
            [
                {
                    $lookup: {
                        from: foreignName,
                        let: {lk: "$k"},
                        pipeline: [
                            {$addFields: {kept: "$fk"}},
                            {$match: {$expr: {$eq: ["$fk", "$$lk"]}}},
                        ],
                        as: "match",
                    },
                },
            ],
            {collation: {locale: "en", strength: 2}},
            "case-insensitive collation",
        );
    });

    it("whole-document match agrees with the projection path across the size gate", function () {
        // Documents below the 16KB gate take the whole-document fast path; the same documents padded
        // beyond it fall back to the projection path. Results must be identical (modulo the padding).
        const foreign = [
            {_id: 10, fk: "a"},
            {_id: 11, fk: "b"},
            {_id: 12, fk: "a"},
            {_id: 13, fk: null},
            {_id: 14 /* fk missing */},
        ];
        reset(localColl, [
            {_id: 1, lk: "a"},
            {_id: 2, lk: "b"},
            {_id: 3, lk: "c"},
            {_id: 4, lk: null},
        ]);

        const pipeline = [
            {
                $lookup: {
                    from: foreignName,
                    let: {lk: "$lk"},
                    pipeline: [{$match: {$expr: {$eq: ["$fk", "$$lk"]}}}],
                    as: "match",
                },
            },
        ];

        setLookupCacheSize(kDefaultCacheSizeBytes);

        reset(
            foreignColl,
            foreign.map((d) => Object.assign({}, d)),
        );
        const fastPath = normalize(localColl.aggregate(pipeline).toArray());

        const pad = "p".repeat(20 * 1024);
        reset(
            foreignColl,
            foreign.map((d) => Object.assign({pad}, d)),
        );
        const stripPad = (results) =>
            results.map((doc) =>
                Object.assign({}, doc, {
                    match: doc.match.map((m) => {
                        const {pad: _ignored, ...rest} = m;
                        return rest;
                    }),
                }),
            );
        const projectionPath = stripPad(normalize(localColl.aggregate(pipeline).toArray()));

        assertArrayEq({
            actual: fastPath,
            expected: projectionPath,
            extraErrorMsg: "whole-document match disagreed with the projection path",
        });
    });
});
