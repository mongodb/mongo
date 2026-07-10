/**
 * Verifies that internal-only 'aggregate' command fields that are set by the router are rejected
 * when supplied by a non-internal client, both on a standalone mongod and on mongos. This test
 * specifically tests the fields 'ifrFlags' and '_translatedForViewlessTimeseries'.
 *
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
});

describe("internal aggregate field rejection on mongos", function () {
    let st;
    let db;
    let coll;

    before(function () {
        st = new ShardingTest({shards: 2});
        db = st.s.getDB(jsTestName());
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
});
