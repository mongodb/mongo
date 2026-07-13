/**
 * Verify the behavior of the '$$SEARCH_META' variable in aggregation sub-pipelines ($unionWith,
 * $lookup, let-variables), including the error cases where the variable is unavailable. Metadata
 * is produced via the `count` operator; the two queries used match a different number of documents
 * so that each pipeline's metadata is distinguishable.
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";

const collName = jsTestName();
const coll = db.getCollection(collName);

const indexName = jsTestName() + "_index";
const numDocs = 9;

// Matches all 9 documents.
const queryAll = {
    index: indexName,
    range: {path: "year", gte: 2000, lt: 2100},
    count: {type: "total"},
};
// Matches the 4 documents with year 2000-2003.
const querySome = {
    index: indexName,
    range: {path: "year", gte: 2000, lt: 2004},
    count: {type: "total"},
};

function assertMetaTotal(doc, expectedTotal) {
    assert(doc.hasOwnProperty("meta"), "expected doc to have meta", {doc});
    assert.eq(Number(doc.meta.count.total), expectedTotal, doc);
}

describe("$$SEARCH_META in sub-pipelines", function () {
    before(function () {
        coll.drop();

        let docs = [];
        for (let i = 0; i < numDocs; i++) {
            docs.push({_id: i, year: 2000 + i});
        }
        assert.commandWorked(coll.insertMany(docs));

        createSearchIndex(coll, {
            name: indexName,
            definition: {mappings: {dynamic: false, fields: {year: {type: "number"}}}},
        });
    });

    after(function () {
        dropSearchIndex(coll, {name: indexName});
        coll.drop();
    });

    // On a sharded collection, $search desugars on mongos into a pipeline where $$SEARCH_META is
    // set by a $setVariableFromSubPipeline stage in the merging half, so the two validation errors
    // (6347901: referenced after a stage with a sub-pipeline; 6347902: referenced without an
    // earlier $search) can fire in either order relative to the standalone desugar. Accept both
    // codes where validation must reject the pipeline.
    const kMetaAccessErrorCodes = [6347901, 6347902];

    it("should fail when referenced after an intervening sub-pipeline stage", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: collName,
                pipeline: [
                    {$search: queryAll},
                    {$unionWith: {pipeline: [{$documents: [{a: 1}]}]}},
                    {$project: {_id: 1, meta: "$$SEARCH_META"}},
                ],
                cursor: {},
            }),
            kMetaAccessErrorCodes,
        );
    });

    it("should resolve to each pipeline's own metadata with $search in local and later child pipeline", function () {
        const results = coll
            .aggregate([
                {$search: queryAll},
                {$project: {_id: 1, meta: "$$SEARCH_META"}},
                {
                    $unionWith: {
                        coll: collName,
                        pipeline: [
                            {$search: querySome},
                            {$project: {_id: {$add: [100, "$_id"]}, meta: "$$SEARCH_META"}},
                        ],
                    },
                },
            ])
            .toArray();

        const outer = results.filter((d) => d._id < 100);
        const inner = results.filter((d) => d._id >= 100);
        assert.eq(outer.length, 9, results);
        assert.eq(inner.length, 4, results);
        outer.forEach((d) => assertMetaTotal(d, 9));
        inner.forEach((d) => assertMetaTotal(d, 4));
    });

    it("should reject or resolve a reference after a child pipeline containing $search, depending on topology", function () {
        const cmd = {
            aggregate: collName,
            pipeline: [
                {$search: queryAll},
                {$unionWith: {coll: collName, pipeline: [{$search: querySome}]}},
                {$project: {meta: "$$SEARCH_META"}},
            ],
            cursor: {},
        };

        if (FixtureHelpers.isSharded(coll)) {
            // On a sharded collection each $search's merging pipeline sets $$SEARCH_META via
            // $setVariableFromSubPipeline, so the reference is valid rather than an error. Which
            // of the two searches' metadata a given document observes depends on the execution
            // order of the merging pipelines, so only assert that every document resolved the
            // variable to one of them.
            const res = assert.commandWorked(db.runCommand(cmd));
            const docs = new DBCommandCursor(db, res).toArray();
            assert.eq(docs.length, 13, docs);
            for (const doc of docs) {
                assert(doc.hasOwnProperty("meta"), "expected doc to have meta", {doc});
                assert.contains(Number(doc.meta.count.total), [9, 4], docs);
            }
        } else {
            assert.commandFailedWithCode(db.runCommand(cmd), 6347901);
        }
    });

    it("should resolve inside a child pipeline even when the parent does not reference it", function () {
        const results = coll
            .aggregate([
                {$search: queryAll},
                {$project: {_id: 1}},
                {
                    $unionWith: {
                        coll: collName,
                        pipeline: [
                            {$search: querySome},
                            {$project: {_id: {$add: [100, "$_id"]}, meta: "$$SEARCH_META"}},
                        ],
                    },
                },
            ])
            .toArray();

        const outer = results.filter((d) => d._id < 100);
        const inner = results.filter((d) => d._id >= 100);
        assert.eq(outer.length, 9, results);
        assert.eq(inner.length, 4, results);
        outer.forEach((d) =>
            assert(!d.hasOwnProperty("meta"), "outer doc should have no meta", {d}),
        );
        inner.forEach((d) => assertMetaTotal(d, 4));
    });

    it("should resolve to each $lookup sub-pipeline's own metadata", function () {
        const results = coll
            .aggregate([
                {$match: {_id: {$in: [0, 1, 2]}}},
                {$project: {_id: 1}},
                {
                    $lookup: {
                        from: collName,
                        pipeline: [{$search: queryAll}, {$project: {meta: "$$SEARCH_META"}}],
                        as: "lookupAll",
                    },
                },
                {
                    $lookup: {
                        from: collName,
                        pipeline: [{$search: querySome}, {$project: {meta: "$$SEARCH_META"}}],
                        as: "lookupSome",
                    },
                },
            ])
            .toArray();

        assert.eq(results.length, 3, results);
        for (const doc of results) {
            assert.eq(doc.lookupAll.length, 9, doc);
            doc.lookupAll.forEach((d) => assertMetaTotal(d, 9));
            assert.eq(doc.lookupSome.length, 4, doc);
            doc.lookupSome.forEach((d) => assertMetaTotal(d, 4));
        }
    });

    it("should fail when only an earlier child pipeline had $search", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: collName,
                pipeline: [
                    {$match: {}},
                    {$unionWith: {coll: collName, pipeline: [{$search: querySome}]}},
                    {$project: {meta: "$$SEARCH_META"}},
                ],
                cursor: {},
            }),
            kMetaAccessErrorCodes,
        );
    });

    it("should fail when referenced before any $search", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: collName,
                pipeline: [
                    {$project: {meta: "$$SEARCH_META"}},
                    {$unionWith: {coll: collName, pipeline: [{$search: querySome}]}},
                ],
                cursor: {},
            }),
            6347902,
        );
    });

    it("should fail when a child pipeline references the parent's metadata", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: collName,
                pipeline: [
                    {$search: queryAll},
                    {$unionWith: {coll: collName, pipeline: [{$project: {meta: "$$SEARCH_META"}}]}},
                ],
                cursor: {},
            }),
            6347902,
        );
    });

    it("should be carried into a $lookup through a let variable", function () {
        const results = coll
            .aggregate([
                {$search: queryAll},
                {$project: {_id: 1}},
                {
                    $lookup: {
                        let: {mySearchMeta: "$$SEARCH_META"},
                        pipeline: [{$documents: [{a: "$$mySearchMeta"}]}],
                        as: "lookup",
                    },
                },
            ])
            .toArray();

        assert.eq(results.length, 9, results);
        for (const doc of results) {
            assert.eq(doc.lookup.length, 1, doc);
            assert.eq(Number(doc.lookup[0].a.count.total), 9, doc);
        }
    });
});
