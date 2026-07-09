/**
 * Verifies that `ifrFlags` is rejected when supplied by an unprivileged (non-internal) client,
 * both on a standalone mongod and at the mongos front door.
 *
 * The transport-level check (error 11516201) in
 * aggregation_request_helper.cpp::validateRequestWithClient fires in all modes. A second,
 * auth-based check (error 13002302) in rpc::readPrivilegedRequestMetadata fires first when
 * the server runs with authentication enabled (production). Both form a layered defense.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("ifrFlags privileged-field rejection on mongod", function () {
    let conn;
    let db;
    let coll;

    before(function () {
        conn = MongoRunner.runMongod();
        db = conn.getDB(jsTestName());
        coll = db.coll;
        assert.commandWorked(coll.insertOne({x: 1}));
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("rejects ifrFlags from an external (non-internal) client", function () {
        const res = db.runCommand({
            aggregate: coll.getName(),
            pipeline: [],
            cursor: {},
            ifrFlags: [{name: "featureFlagSerializeForTest", value: true}],
        });
        assert.commandFailedWithCode(res, 11516201);
    });
});

describe("ifrFlags privileged-field rejection on mongos", function () {
    let st;
    let db;
    let coll;

    before(function () {
        st = new ShardingTest({shards: 1});
        db = st.s.getDB(jsTestName());
        coll = db.coll;
        assert.commandWorked(coll.insertOne({x: 1}));
    });

    after(function () {
        st.stop();
    });

    it("rejects ifrFlags from an external (non-internal) client at the mongos front door", function () {
        const res = db.runCommand({
            aggregate: coll.getName(),
            pipeline: [],
            cursor: {},
            ifrFlags: [{name: "featureFlagSerializeForTest", value: true}],
        });
        assert.commandFailedWithCode(res, 11516201);
    });
});
