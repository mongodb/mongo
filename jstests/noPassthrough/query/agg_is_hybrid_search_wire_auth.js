/**
 * Verifies that the internal-only 'aggregate' command field '$_isHybridSearch', which is set by the
 * router for a desugared $rankFusion/$scoreFusion pipeline, is rejected when supplied by a
 * non-internal client, both on a standalone mongod and on a sharded cluster. Also verifies that the
 * pipelines which legitimately cause the server to set the field still succeed.
 *
 * @tags: [
 *   featureFlagSearchHybridScoringFull
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kIsHybridSearchErrorCode = 13212500;

const rankFusionStage = {
    $rankFusion: {
        input: {
            pipelines: {
                ascending: [{$sort: {x: 1}}],
                descending: [{$sort: {x: -1}}],
            },
        },
    },
};

const scoreFusionStage = {
    $scoreFusion: {
        input: {
            pipelines: {
                a: [{$score: {score: "$x", normalization: "minMaxScaler"}}],
                b: [{$score: {score: "$x", normalization: "minMaxScaler"}}],
            },
            normalization: "none",
        },
        combination: {method: "avg"},
    },
};

const hybridSearchStages = [
    {name: "$rankFusion", stage: rankFusionStage},
    {name: "$scoreFusion", stage: scoreFusionStage},
];

function assertHybridSearchFieldRejected(db, collName, pipeline = []) {
    assert.commandFailedWithCode(
        db.runCommand({
            aggregate: collName,
            pipeline,
            cursor: {},
            $_isHybridSearch: true,
        }),
        kIsHybridSearchErrorCode,
        "external client should be rejected setting '$_isHybridSearch'",
        {pipeline},
    );

    // 'explain' is a separate command entry point.
    assert.commandFailedWithCode(
        db.runCommand({
            explain: {
                aggregate: collName,
                pipeline,
                cursor: {},
                $_isHybridSearch: true,
            },
            verbosity: "queryPlanner",
        }),
        kIsHybridSearchErrorCode,
        "external client should be rejected setting '$_isHybridSearch' on explain",
        {pipeline},
    );
}

// A user supplying the field together with a real hybrid search pipeline is the case the guard
// exists for, since that request is otherwise indistinguishable from one the router produced.
function assertHybridSearchFieldRejectedForAllPipelines(db, collName) {
    assertHybridSearchFieldRejected(db, collName);
    for (const {stage} of hybridSearchStages) {
        assertHybridSearchFieldRejected(db, collName, [stage]);
    }
}

// A user has no legitimate reason to name an internal field at all, so even 'false' is rejected.
function assertHybridSearchFieldFalseRejected(db, collName) {
    assert.commandFailedWithCode(
        db.runCommand({
            aggregate: collName,
            pipeline: [],
            cursor: {},
            $_isHybridSearch: false,
        }),
        kIsHybridSearchErrorCode,
        "external client should be rejected setting '$_isHybridSearch: false'",
    );
}

function assertHybridSearchPipelinesAccepted(db, collName) {
    for (const {name, stage} of hybridSearchStages) {
        assert.commandWorked(
            db.runCommand({
                aggregate: collName,
                pipeline: [stage],
                cursor: {},
            }),
            `${name} should be accepted from an external client`,
        );

        assert.commandWorked(
            db.runCommand({
                explain: {
                    aggregate: collName,
                    pipeline: [stage],
                    cursor: {},
                },
                verbosity: "queryPlanner",
            }),
            `explained ${name} should be accepted from an external client`,
        );
    }
}

describe("$_isHybridSearch rejection on mongod", function () {
    let conn;
    let db;
    let coll;

    before(function () {
        conn = MongoRunner.runMongod();
        db = conn.getDB(jsTestName());
        coll = db.coll;
        coll.drop();
        assert.commandWorked(coll.insertOne({x: 1}));
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("rejects '$_isHybridSearch' from an external client", function () {
        assertHybridSearchFieldRejectedForAllPipelines(db, coll.getName());
    });

    it("rejects '$_isHybridSearch: false' from an external client", function () {
        assertHybridSearchFieldFalseRejected(db, coll.getName());
    });

    it("accepts a hybrid search pipeline from an external client", function () {
        assertHybridSearchPipelinesAccepted(db, coll.getName());
    });
});

describe("$_isHybridSearch rejection on mongos", function () {
    let st;
    let db;
    let shardDB;
    let coll;

    before(function () {
        st = new ShardingTest({shards: 2});
        db = st.s.getDB(jsTestName());
        shardDB = st.shard0.getDB(jsTestName());
        coll = db.coll;
        coll.drop();
        assert.commandWorked(coll.insertOne({x: 1}));
    });

    after(function () {
        st.stop();
    });

    it("rejects '$_isHybridSearch' from an external client on the router", function () {
        assertHybridSearchFieldRejectedForAllPipelines(db, coll.getName());
    });

    // An external client can connect directly to a shard, which is also the node that legitimately
    // receives the field from the router.
    it("rejects '$_isHybridSearch' from an external client on a shard", function () {
        assertHybridSearchFieldRejectedForAllPipelines(shardDB, coll.getName());
    });

    it("rejects '$_isHybridSearch: false' from an external client", function () {
        assertHybridSearchFieldFalseRejected(db, coll.getName());
        assertHybridSearchFieldFalseRejected(shardDB, coll.getName());
    });

    // The router sets '$_isHybridSearch' on the user's own request when it sees a hybrid search
    // stage, so rejecting the field must not reject the requests that legitimately produce it.
    it("accepts a hybrid search pipeline from an external client", function () {
        assertHybridSearchPipelinesAccepted(db, coll.getName());
    });

    it("accepts a hybrid search pipeline on a view from an external client", function () {
        assert.commandWorked(db.createView("collView", coll.getName(), [{$match: {x: {$gte: 0}}}]));
        try {
            assertHybridSearchPipelinesAccepted(db, "collView");
        } finally {
            db.collView.drop();
        }
    });
});
