/**
 * Tests how express code path works with projections.
 * @tags: [
 *   requires_fcv_83,
 *   # "Refusing to run a test that issues an aggregation command with explain because it may return
 *   # incomplete results"
 *   does_not_support_stepdowns,
 *   # "Explain for the aggregate command cannot run within a multi-document transaction"
 *   does_not_support_transactions,
 * ]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {runExpressTest} from "jstests/libs/query/express_utils.js";

const coll = db.getCollection('express_coll_projection');

let isSharded = false;
let expectedNonZeroFetchCountWhenCovered = false;
function recreateCollWith(documents) {
    assertDropAndRecreateCollection(db, coll.getName());
    assert.commandWorked(coll.insert(documents));
    // TODO SERVER-108344 Remove when shard filter is supported
    isSharded = FixtureHelpers.isSharded(coll);
    expectedNonZeroFetchCountWhenCovered = isSharded ? true : false;
}

describe("Express path supports simple projections", () => {
    const docs = [
        {_id: 0, a: 0, b: 0},
        {_id: 1, a: "string"},
        {_id: 2, a: {bar: 1}},
        {_id: 3, a: null},
        {_id: 4, a: [1, 2, 3]},
    ];

    before(() => recreateCollWith(docs));
    beforeEach(() => coll.dropIndexes());

    it("supports exclusion projection", () => {
        assert.commandWorked(coll.createIndex({a: 1}));
        runExpressTest({
            coll,
            filter: {a: 0},
            project: {_id: 0, b: 0},
            limit: 1,
            result: [{a: 0}],
            usesExpress: !isSharded
        });
    });

    it("can cover inclusion projection with limit 1", () => {
        assert.commandWorked(coll.createIndex({a: 1, extra: 1, b: 1}));
        runExpressTest({
            coll,
            filter: {a: 0},
            project: {b: 1, _id: 0},
            limit: 1,
            result: [{b: 0}],
            usesExpress: !isSharded,
            expectedNonZeroFetchCountWhenCovered,
        });
    });
    it("can cover inclusion projection without limit 1 if there is a matching unique index present",
       () => {
           assert.commandWorked(coll.createIndex({a: 1, extra: 1, b: 1}));
           runExpressTest({
               coll,
               filter: {a: 0},
               project: {b: 1, _id: 0},
               result: [{b: 0}],
               usesExpress: false,  // no unique index
               expectedNonZeroFetchCountWhenCovered,
           });
           if (!isSharded) {
               assert.commandWorked(coll.createIndex({a: 1}, {unique: true}));
               runExpressTest({
                   coll,
                   filter: {a: 0},
                   project: {b: 1, _id: 0},
                   result: [{b: 0}],
                   usesExpress: true,  // index
                   expectedNonZeroFetchCountWhenCovered,
               });
           }
       });
});

describe("Express path correctly handles multi-key indexes when covering projections", () => {
    const docs = [
        {_id: 0, a: 0, b: 0, c: [1, 2]},
        {_id: 1, a: "string", b: "string", c: [3, 4]},
        {_id: 2, a: "string2", b: 5, c: ["stringA", "stringB"]},
        {_id: 3, a: 1, b: "string3", c: []},
    ];

    beforeEach(() => recreateCollWith(docs));

    it("covers projections with multi-key index if multi-key path is not involved", () => {
        assert.commandWorked(coll.createIndex({a: 1, extra1: 1, c: 1, extra2: 1, b: 1}));
        runExpressTest({
            coll,
            filter: {a: "string"},
            project: {a: 1, b: 1, _id: 0},
            limit: 1,
            result: [{a: "string", b: "string"}],
            usesExpress: !isSharded,
            expectedNonZeroFetchCountWhenCovered,
        });
        runExpressTest({
            coll,
            filter: {a: "string"},
            project: {a: 1, b: 1, _id: 0},
            result: [{a: "string", b: "string"}],
            usesExpress: false,
            expectedNonZeroFetchCountWhenCovered,
        });
        if (!isSharded) {
            assert.commandWorked(coll.createIndex({a: 1}, {unique: true}));
            runExpressTest({
                coll,
                filter: {a: "string"},
                project: {a: 1, b: 1, _id: 0},
                result: [{a: "string", b: "string"}],
                usesExpress: true,
                expectedNonZeroFetchCountWhenCovered,
            });
        }
    });
    it("covers projections with multi-key index if filter is on the multi-key path", () => {
        recreateCollWith([{a: [1, 2], b: 3, c: 4}]);
        assert.commandWorked(coll.createIndex({a: 1, extra1: 1, c: 1, extra2: 1, b: 1}));
        runExpressTest({
            coll,
            filter: {a: 2},
            project: {b: 1, c: 1, _id: 0},
            limit: 1,
            result: [{b: 3, c: 4}],
            usesExpress: !isSharded,
            expectedNonZeroFetchCountWhenCovered,
        });
    });
    it("does not cover projections with multi-key index if multi-key path is involved", () => {
        assert.commandWorked(coll.createIndex({a: 1, extra1: 1, c: 1, extra2: 1, b: 1}));
        runExpressTest({
            coll,
            filter: {a: "string"},
            project: {a: 1, b: 1, c: 1, _id: 0},
            limit: 1,
            result: [{a: "string", b: "string", c: [3, 4]}],
            usesExpress: !isSharded,
            expectedNonZeroFetchCountWhenCovered: true,
        });
    });
    it("does not use express if complete multi-key field is required for filter", () => {
        recreateCollWith([{a: [1, 2], b: 3, c: 4}]);
        assert.commandWorked(coll.createIndex({a: 1, extra1: 1, c: 1, extra2: 1, b: 1}));

        runExpressTest({
            coll,
            filter: {a: [1, 2]},
            project: {b: 1, _id: 0},
            limit: 1,
            result: [{b: 3}],
            usesExpress: false,
            expectedNonZeroFetchCountWhenCovered: true,
        });
    });

    after(() => coll.drop());
});

describe("Express path handles collation when covering projections", () => {
    const docs = [];
    let index = 0;
    for (let aString of [false, true]) {
        for (let bString of [false, true]) {
            for (let cString of [false, true]) {
                docs.push({
                    a: (aString ? "" : 0) + index++,
                    b: (bString ? "" : 0) + index++,
                    c: (cString ? "" : 0) + index++,
                });
            }
        }
    }

    const caseInsensitive = {locale: "en_US", strength: 2};

    before(() => {
        recreateCollWith(docs);
        assert.commandWorked(coll.createIndex({a: 1, extra1: 1, c: 1, extra2: 1, b: 1},
                                              {collation: caseInsensitive}));
        assert.commandWorked(coll.createIndex({a: 1}, {collation: caseInsensitive}));
    });

    it("covers projections when collation is not relevant for filter value", () => {
        runExpressTest({
            coll,
            filter: {a: 0},
            project: {a: 1, _id: 0},
            limit: 1,
            collation: caseInsensitive,
            result: [{a: 0}],
            usesExpress: !isSharded,
            expectedNonZeroFetchCountWhenCovered,
        });
    });

    it("does not cover projections when collation is relevant for filter value", () => {
        runExpressTest({
            coll,
            filter: {a: "12"},
            project: {a: 1, _id: 0},
            limit: 1,
            collation: caseInsensitive,
            result: [{a: "12"}],
            usesExpress: !isSharded,
            expectedNonZeroFetchCountWhenCovered: true,
        });
    });

    it("does not cover projections with collation when have other values", () => {
        runExpressTest({
            coll,
            filter: {a: 9},
            project: {a: 1, b: 1, _id: 0},
            limit: 1,
            collation: caseInsensitive,
            result: [{a: 9, b: "10"}],
            usesExpress: !isSharded,
            expectedNonZeroFetchCountWhenCovered: true,
        });
    });

    after(() => coll.drop());
});

describe("Express path includes projection into explain", () => {
    const docs = [{_id: 0, a: 1, b: 2}, {_id: 3, a: 4, b: 5}];

    before(() => {
        recreateCollWith(docs);
    });

    it("includes projection in explain output when using user index", () => {
        if (isSharded) {
            return;
        }

        assert.commandWorked(coll.createIndex({a: 1}, {unique: true}));
        assert.commandWorked(coll.createIndex({a: 1, b: 1}));

        const explainCovered =
            getWinningPlanFromExplain(coll.find({a: 1}, {b: 1, _id: 0}).explain());

        assert.eq(explainCovered.stage, "EXPRESS_IXSCAN", explainCovered);
        assert.eq(explainCovered.projection, {b: 1, _id: 0}, explainCovered);
        assert.eq(explainCovered.projectionCovered,
                  !expectedNonZeroFetchCountWhenCovered,
                  explainCovered);

        const explainNotCovered = getWinningPlanFromExplain(coll.find({a: 1}, {_id: 0}).explain());

        assert.eq(explainNotCovered.stage, "EXPRESS_IXSCAN", explainNotCovered);
        assert.eq(explainNotCovered.projection, {_id: 0}, explainNotCovered);
        assert.eq(explainNotCovered.projectionCovered, false, explainNotCovered);
    });
    it("includes projection in explain output when using _id index", () => {
        const explain = getWinningPlanFromExplain(coll.find({_id: 0}, {b: 1, _id: 0}).explain());

        assert.eq(explain.stage, "EXPRESS_IXSCAN", explain);
        assert.eq(explain.projection, {b: 1, _id: 0}, explain);
        assert.eq(explain.projectionCovered, false, explain);
    });

    after(() => coll.drop());
});
