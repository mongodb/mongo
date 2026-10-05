/**
 * Verifies that internal-only 'aggregate' command fields that are set by the router are rejected
 * when supplied by a non-internal client, both on a standalone mongod and on mongos.
 *
 *  @tags: [requires_fcv_82] # $scoreFusion is enabled by default on FCV >= 8.2
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const internalFieldTestCases = [
    {
        name: "ifrFlags",
        field: {ifrFlags: [{name: "featureFlagSerializeForTest", value: true}]},
        errorCode: 11516201,
    },
    {
        name: "$_translatedForViewlessTimeseries",
        field: {$_translatedForViewlessTimeseries: true},
        errorCode: 13088600,
    },
];

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

function assertInternalFieldsRejected(db, coll) {
    for (const {name, field, errorCode} of internalFieldTestCases) {
        const res = db.runCommand({
            aggregate: coll.getName(),
            pipeline: [],
            cursor: {},
            ...field,
        });
        assert.commandFailedWithCode(
            res,
            errorCode,
            `external client should be rejected setting '${name}'`,
        );
    }
}

// Unlike the fields in 'internalFieldTestCases', the router sets '$_isHybridSearch' on the user's
// own request when the pipeline contains a hybrid search stage. These helpers therefore assert both
// halves: an external client cannot supply the field, and the requests that legitimately cause the
// server to set it still succeed.
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

    // 'explain' is a separate command entry point, with its own call to validate().
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

    // A user has no legitimate reason to name an internal field at all, so even 'false' is rejected.
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

// A user supplying the flag together with a real hybrid search pipeline is the case the guard
// exists for, since that request is otherwise indistinguishable from one the router produced.
function assertHybridSearchFieldRejectedForAllPipelines(db, collName) {
    assertHybridSearchFieldRejected(db, collName);
    for (const {stage} of hybridSearchStages) {
        assertHybridSearchFieldRejected(db, collName, [stage]);
    }
}

describe("internal aggregate field rejection on mongod", function () {
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

    it("rejects internal only aggregate fields from a non-internal client", function () {
        assertInternalFieldsRejected(db, coll);
    });

    it("accepts '$_translatedForViewlessTimeseries: false' from an external client", function () {
        assert.commandWorked(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [],
                cursor: {},
                $_translatedForViewlessTimeseries: false,
            }),
        );
    });

    describe("$_isHybridSearch", function () {
        it("rejects '$_isHybridSearch' from an external client", function () {
            assertHybridSearchFieldRejectedForAllPipelines(db, coll.getName());
        });
    });
});

describe("internal aggregate field rejection on mongos", function () {
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

    it("rejects internal only aggregate fields from a non-internal client", function () {
        assertInternalFieldsRejected(db, coll);
    });

    describe("$_isHybridSearch", function () {
        it("rejects '$_isHybridSearch' from an external client on the router", function () {
            assertHybridSearchFieldRejectedForAllPipelines(db, coll.getName());
        });

        // An external client can connect directly to a shard, which is also the node that
        // legitimately receives the field from the router.
        it("rejects '$_isHybridSearch' from an external client on a shard", function () {
            assertHybridSearchFieldRejectedForAllPipelines(shardDB, coll.getName());
        });

        // The router sets '$_isHybridSearch' on the user's own request when it sees a hybrid search
        // stage, so rejecting the field must not reject the requests that legitimately produce it.
        it("accepts a hybrid search pipeline from an external client", function () {
            for (const {name, stage} of hybridSearchStages) {
                assert.commandWorked(
                    db.runCommand({
                        aggregate: coll.getName(),
                        pipeline: [stage],
                        cursor: {},
                    }),
                    `${name} should be accepted from an external client`,
                );
            }
        });

        it("accepts an explained hybrid search pipeline from an external client", function () {
            for (const {name, stage} of hybridSearchStages) {
                assert.commandWorked(
                    db.runCommand({
                        explain: {
                            aggregate: coll.getName(),
                            pipeline: [stage],
                            cursor: {},
                        },
                        verbosity: "queryPlanner",
                    }),
                    `explained ${name} should be accepted from an external client`,
                );
            }
        });

        it("accepts a hybrid search pipeline on a view from an external client", function () {
            assert.commandWorked(
                db.createView("collView", coll.getName(), [{$match: {x: {$gte: 0}}}]),
            );
            try {
                for (const {name, stage} of hybridSearchStages) {
                    assert.commandWorked(
                        db.runCommand({
                            aggregate: "collView",
                            pipeline: [stage],
                            cursor: {},
                        }),
                        `${name} on a view should be accepted from an external client`,
                    );
                }
            } finally {
                db.collView.drop();
            }
        });
    });
});
