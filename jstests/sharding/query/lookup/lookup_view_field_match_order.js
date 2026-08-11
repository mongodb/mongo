/**
 * Tests the placement of the join $match in a localField/foreignField $lookup whose 'from' is a
 * view. The $match must run after the view's stages, so the view can compute the field being
 * joined on, and before any user pipeline, which may remove that field.
 *
 * The failures these cases cover live in the router -> shard round trip of a view-resolved $lookup:
 * the router rewrites 'from' to the backing collection and moves the view's stages into 'pipeline',
 * which changes what the spec means unless the $match's position is carried along with it. Each
 * case therefore runs against a standalone mongod as a control -- no router, so no rewrite -- and
 * then through a router.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_fcv_90,
 * ]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// The view computes the foreign field 'b', so the join predicate is only satisfiable after the
// view's stages have run.
const kViewPipeline = [{$addFields: {b: 0}}];

function setUpFixture(testDB, {shardForeign = false} = {}) {
    testDB.v.drop();
    testDB.vMultiStage.drop();
    testDB.topSeller.drop();
    testDB.foreign.drop();
    testDB.local.drop();
    testDB.sales.drop();
    testDB.reports.drop();

    assert.commandWorked(testDB.foreign.insert({}));
    if (shardForeign) {
        assert.commandWorked(
            testDB
                .getSiblingDB("admin")
                .runCommand({shardCollection: testDB.foreign.getFullName(), key: {_id: 1}}),
        );
    }
    assert.commandWorked(testDB.createView("v", "foreign", kViewPipeline));
    // A view whose stages the $match must follow across more than one stage.
    assert.commandWorked(
        testDB.createView("vMultiStage", "foreign", [{$addFields: {t: 1}}, {$addFields: {b: 0}}]),
    );
    assert.commandWorked(testDB.local.insert({a: 0}));

    // A view whose stages select which documents exist at all, so running the join $match ahead of
    // them changes the result set rather than just the field being matched on.
    assert.commandWorked(
        testDB.sales.insertMany([{product: "widget"}, {product: "widget"}, {product: "gizmo"}]),
    );
    assert.commandWorked(
        testDB.createView("topSeller", "sales", [{$sortByCount: "$product"}, {$limit: 1}]),
    );
    assert.commandWorked(testDB.reports.insert({product: "gizmo"}));
}

// The view reduces its input to the single top-selling product, "widget". "gizmo" must not join: a
// join $match placed ahead of the view's stages would filter to the "gizmo" sales first and make it
// the top seller.
function assertFilteringViewJoinsNothing(testDB) {
    const res = testDB.reports
        .aggregate([
            {$lookup: {from: "topSeller", localField: "product", foreignField: "_id", as: "t"}},
        ])
        .toArray();
    assert.eq(0, res[0].t.length, "gizmo is not the top seller and must not join", {res});
}

// The same filtering view, but with a user pipeline alongside localField/foreignField. This takes
// the general StageParams construction path rather than the pre-resolved-view one, so it covers a
// different derivation of the join $match's position than the case above.
function assertFilteringViewWithUserPipelineJoinsNothing(testDB) {
    const res = testDB.reports
        .aggregate([
            {
                $lookup: {
                    from: "topSeller",
                    localField: "product",
                    foreignField: "_id",
                    pipeline: [{$addFields: {tag: 1}}],
                    as: "t",
                },
            },
        ])
        .toArray();
    assert.eq(0, res[0].t.length, "gizmo is not the top seller and must not join", {res});
}

// localField/foreignField-only syntax. The view's stages are the entire subpipeline, so if the
// $match's position is not carried to the shard the join predicate runs before the view computes
// 'b' and matches nothing.
function assertFieldsOnlySyntaxJoins(testDB) {
    const res = testDB.local
        .aggregate([{$lookup: {from: "v", localField: "a", foreignField: "b", as: "c"}}])
        .toArray();
    assert.eq(1, res[0].c.length, `expected the view's document to join. ${tojson(res)}`);
}

// localField/foreignField plus a user pipeline. The $match belongs after the view's stages but
// before the user pipeline, which unsets the very field being joined on. A $match placed after the
// user pipeline sees no 'b' and matches nothing.
function assertFieldsAndUserPipelineSyntaxJoins(testDB) {
    const res = testDB.local
        .aggregate([
            {
                $lookup: {
                    from: "v",
                    localField: "a",
                    foreignField: "b",
                    as: "c",
                    pipeline: [{$unset: "b"}, {$addFields: {extra: 1}}],
                },
            },
        ])
        .toArray();
    assert.eq(
        1,
        res[0].c.length,
        `the join $match must run before the user pipeline unsets the foreign field. ${tojson(res)}`,
    );
    assert.eq(1, res[0].c[0].extra, `user pipeline did not run. ${tojson(res)}`);
}

// A multi-stage view pipeline, so the $match's position is not simply index 1.
function assertMultiStageViewPipelineJoins(testDB) {
    const res = testDB.local
        .aggregate([{$lookup: {from: "vMultiStage", localField: "a", foreignField: "b", as: "c"}}])
        .toArray();
    assert.eq(1, res[0].c.length, `expected the view's document to join. ${tojson(res)}`);
    assert.eq(1, res[0].c[0].t, `earlier view stage did not run. ${tojson(res)}`);
}

// The view-resolved shape the router puts on the wire, sent straight to a mongod. This pins the
// shard-side parse boundary on its own: 'from' is the concrete backing collection, the view's
// stages arrive in 'pipeline', and $_internalFieldMatchPipelineIdx says the join $match is the
// subpipeline's second stage. Only meaningful against a mongod, which is what a shard is here.
function viewResolvedSpec(fieldMatchPipelineIdx) {
    return {
        from: "foreign",
        localField: "a",
        foreignField: "b",
        as: "c",
        pipeline: kViewPipeline,
        $_internalFromIsAView: true,
        $_internalFieldMatchPipelineIdx: NumberLong(fieldMatchPipelineIdx),
    };
}

function runInternalClientAggregate(internalTestDB, lookupSpec) {
    const res = assert.commandWorked(
        internalTestDB.runCommand({
            aggregate: "local",
            pipeline: [{$lookup: lookupSpec}],
            cursor: {},
            // internalClient connections must specify an explicit writeConcern on every command
            // that accepts one.
            writeConcern: {w: "majority"},
        }),
    );
    return res.cursor.firstBatch;
}

function assertViewResolvedSpecFromRouterJoins(internalTestDB) {
    const batch = runInternalClientAggregate(internalTestDB, viewResolvedSpec(1));
    assert.eq(
        1,
        batch[0].c.length,
        `the router's $_internalFieldMatchPipelineIdx was not honored. ${tojson(batch)}`,
    );
}

function assertViewResolvedSpecWithIdxZeroFindsNothing(internalTestDB) {
    const batch = runInternalClientAggregate(internalTestDB, viewResolvedSpec(0));
    assert.eq(
        0,
        batch[0].c.length,
        `a join $match ahead of the view's stages cannot match a field the view has not computed ` +
            `yet. ${tojson(batch)}`,
    );
}

describe("$lookup against a view runs the join $match after the view's stages", function () {
    // Control: every shape must already be correct with no router in the picture. A failure here is
    // a different bug from the router -> shard rewrite under test.
    describe("on a standalone mongod", function () {
        let conn;
        let testDB;
        let internalTestDB;

        before(function () {
            conn = MongoRunner.runMongod({});
            testDB = conn.getDB(jsTestName());
            setUpFixture(testDB);

            const internalConn = new Mongo(conn.host);
            assert.commandWorked(
                internalConn.getDB("admin").runCommand({
                    hello: 1,
                    internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)},
                }),
            );
            internalTestDB = internalConn.getDB(jsTestName());
        });

        after(function () {
            MongoRunner.stopMongod(conn);
        });

        it("joins with localField/foreignField only", function () {
            assertFieldsOnlySyntaxJoins(testDB);
        });

        it("joins with localField/foreignField and a user pipeline", function () {
            assertFieldsAndUserPipelineSyntaxJoins(testDB);
        });

        it("joins across a multi-stage view pipeline", function () {
            assertMultiStageViewPipelineJoins(testDB);
        });

        it("does not join a document the view's stages filter out", function () {
            assertFilteringViewJoinsNothing(testDB);
        });

        it("does not join a filtered-out document when a user pipeline is present", function () {
            assertFilteringViewWithUserPipelineJoinsNothing(testDB);
        });

        it("honors the router's view-resolved spec", function () {
            assertViewResolvedSpecFromRouterJoins(internalTestDB);
        });

        it("places the join $match first when the router's index says 0", function () {
            assertViewResolvedSpecWithIdxZeroFindsNothing(internalTestDB);
        });

        it("rejects the router's internal view-resolution fields in a user request", function () {
            assert.commandFailedWithCode(
                testDB.runCommand({
                    aggregate: "local",
                    pipeline: [{$lookup: viewResolvedSpec(1)}],
                    cursor: {},
                }),
                5491300,
            );
        });
    });

    // The same shapes through a router, which resolves the view and dispatches the rewritten
    // $lookup to a shard.
    describe("through a router", function () {
        let st;
        let testDB;

        before(function () {
            st = new ShardingTest({shards: 2});
            testDB = st.s.getDB(jsTestName());
            assert.commandWorked(st.s.adminCommand({enableSharding: testDB.getName()}));
        });

        after(function () {
            st.stop();
        });

        for (const shardForeign of [false, true]) {
            const backing = shardForeign
                ? "with a sharded backing collection"
                : "with an unsharded backing collection";
            describe(backing, function () {
                before(function () {
                    setUpFixture(testDB, {shardForeign: shardForeign});
                });

                it("joins with localField/foreignField only", function () {
                    assertFieldsOnlySyntaxJoins(testDB);
                });

                it("joins with localField/foreignField and a user pipeline", function () {
                    assertFieldsAndUserPipelineSyntaxJoins(testDB);
                });

                it("joins across a multi-stage view pipeline", function () {
                    assertMultiStageViewPipelineJoins(testDB);
                });

                it("does not join a document the view's stages filter out", function () {
                    assertFilteringViewJoinsNothing(testDB);
                });

                it("does not join a filtered-out document when a user pipeline is present", function () {
                    assertFilteringViewWithUserPipelineJoinsNothing(testDB);
                });
            });
        }
    });
});
