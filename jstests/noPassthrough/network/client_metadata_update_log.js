/**
 * Test that verifies clientMetadataUpdate logging behavior.
 * @tags: [requires_scripting, requires_sharding]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kTestCid = "test-cid-12345";
const kDefaultClientUpdateDoc = {
    driver: {name: "TestDriver", version: "1.0"},
    cid: kTestCid,
    application: {name: "TestApp"},
    unknown_field: {name: "unknown_value"},
};

// Raise the global rate limit so back-to-back clientUpdate events in these tests are all logged
// at Info severity. The default (50/sec => 20ms between events) is small enough that consecutive
// subtests on a fast host land in the same bucket and get demoted to Debug(2), making the log
// entry invisible to filters.
const kSetParameter = {clientMetadataUpdateLogRatePerSec: 500};

function sendClientMetadataUpdate(conn, doc = kDefaultClientUpdateDoc) {
    assert.commandWorked(conn.adminCommand({hello: 1, clientUpdate: doc}));
}

function getHandshakeClientMetadataLogCount(conn) {
    return checkLog.getFilteredLogMessages(conn, 51800, {}).length;
}

function getClientMetadataUpdateLogCount(conn) {
    return checkLog.getFilteredLogMessages(conn, 51817, {}).length;
}

function defineTests(getConn) {
    it("handshake logs client metadata but not clientUpdate", function () {
        const conn = getConn();
        assert.gt(
            getHandshakeClientMetadataLogCount(conn),
            0,
            "Handshake client metadata log (51800) must be present",
        );
        assert.eq(
            getClientMetadataUpdateLogCount(conn),
            0,
            "Client metadata update log (51817) must not be present on handshake",
        );
    });

    it("clientUpdate logs the provided document", function () {
        const conn = getConn();
        sendClientMetadataUpdate(conn);

        const logMessages = checkLog.getFilteredLogMessages(conn, 51817, {});
        assert.eq(
            logMessages.length,
            1,
            "Client metadata update log (51817) must be logged exactly once for valid metadata",
        );

        const last = logMessages[logMessages.length - 1];
        assert.eq(last.msg, "client metadata", "Unexpected log message", {last});
        assert.docEq(
            last.attr.doc,
            kDefaultClientUpdateDoc,
            "clientUpdate must log the document exactly as provided",
            {last},
        );
    });

    it("clientUpdate does not appear in slow query log", function () {
        const conn = getConn();
        const countBefore = getClientMetadataUpdateLogCount(conn);
        sendClientMetadataUpdate(conn);
        assert.eq(
            getClientMetadataUpdateLogCount(conn),
            countBefore + 1,
            "Client metadata update log (51817) must be logged before slow query test",
        );

        const coll = conn.getCollection("test.foo");
        coll.insert({_id: 1});
        coll.count({
            $where: function () {
                sleep(3000);
                return true;
            },
        });

        const slowQueryMessages = checkLog.getFilteredLogMessages(conn, 51803, {});
        const fooSlowQueries = slowQueryMessages.filter(
            (m) => m.attr && m.attr.command && m.attr.command.count === "foo",
        );
        assert.gt(fooSlowQueries.length, 0, "Slow query log (51803) for test.foo must be present");

        fooSlowQueries.forEach((m) => {
            assert.neq(
                m.attr.appName,
                kDefaultClientUpdateDoc.application.name,
                "Slow query appName must not come from clientUpdate",
                {m},
            );
        });
    });

    it("clientUpdate without cid is logged", function () {
        const conn = getConn();
        const countBefore = getClientMetadataUpdateLogCount(conn);
        sendClientMetadataUpdate(conn, {driver: {name: "TestDriver", version: "1.0"}});
        assert.eq(
            getClientMetadataUpdateLogCount(conn),
            countBefore + 1,
            "Client metadata update log (51817) must increase when cid is missing",
        );
    });
}

describe("clientMetadataUpdate logging on mongod", function () {
    let mongod;
    before(function () {
        mongod = MongoRunner.runMongod({useLogFiles: true, setParameter: kSetParameter});
        assert.neq(null, mongod, "mongod was unable to start up");
    });
    after(function () {
        MongoRunner.stopMongod(mongod);
    });
    defineTests(() => new Mongo("127.0.0.1:" + mongod.port));
});

describe("clientMetadataUpdate logging on mongos", function () {
    let st;
    before(function () {
        st = new ShardingTest({
            shards: 1,
            mongos: 1,
            other: {
                mongosOptions: {useLogFiles: true, setParameter: kSetParameter},
                shardOptions: {setParameter: kSetParameter},
            },
        });
    });
    after(function () {
        st.stop();
    });
    defineTests(() => new Mongo("127.0.0.1:" + st.s0.port));
});
