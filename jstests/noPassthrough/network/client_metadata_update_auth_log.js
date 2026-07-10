/**
 * Verifies that the clientUpdate log entry (LOGV2 51817) records the connection's
 * authentication status in the "auth" attribute.
 * @tags: [requires_scripting, requires_auth, requires_sharding]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kClientUpdate = {driver: {name: "TestDriver", version: "1.0"}, cid: "auth-log-cid"};
const kSetParameter = {clientMetadataUpdateLogRatePerSec: 500};

function getClientMetadataUpdateLogs(conn) {
    return checkLog.getFilteredLogMessages(conn, 51817, {});
}

function defineAuthTests(getAddr, getLogConn) {
    it("unauthenticated connection logs auth:false", function () {
        const logConn = getLogConn();
        const countBefore = getClientMetadataUpdateLogs(logConn).length;
        const conn = new Mongo(getAddr());
        assert.commandWorked(conn.adminCommand({hello: 1, clientUpdate: kClientUpdate}));

        const logs = getClientMetadataUpdateLogs(logConn);
        assert.eq(logs.length, countBefore + 1, "clientUpdate log (51817) must be emitted", {logs});
        const entry = logs[logs.length - 1];
        assert.eq(entry.attr.auth, false, "unauthenticated connection must log auth:false", {
            entry,
        });
    });

    it("authenticated connection logs auth:true", function () {
        const logConn = getLogConn();
        const countBefore = getClientMetadataUpdateLogs(logConn).length;
        const conn = new Mongo(getAddr());
        assert(conn.getDB("admin").auth("admin", "pwd"), "authentication must succeed");
        assert.commandWorked(conn.adminCommand({hello: 1, clientUpdate: kClientUpdate}));

        const logs = getClientMetadataUpdateLogs(logConn);
        assert.eq(logs.length, countBefore + 1, "clientUpdate log (51817) must be emitted", {logs});
        const entry = logs[logs.length - 1];
        assert.eq(entry.attr.auth, true, "authenticated connection must log auth:true", {entry});
    });
}

describe("clientUpdate authenticated attribute on mongod", function () {
    let mongod;

    before(function () {
        mongod = MongoRunner.runMongod({
            auth: "",
            useLogFiles: true,
            setParameter: kSetParameter,
        });
        assert.neq(null, mongod, "mongod was unable to start up");

        const adminDb = mongod.getDB("admin");
        adminDb.createUser({user: "admin", pwd: "pwd", roles: ["root"]});
        assert(adminDb.auth("admin", "pwd"), "default connection auth must succeed");
    });

    after(function () {
        MongoRunner.stopMongod(mongod);
    });

    defineAuthTests(
        () => "127.0.0.1:" + mongod.port,
        () => mongod,
    );
});

describe("clientUpdate authenticated attribute on mongos", function () {
    let st;

    before(function () {
        st = new ShardingTest({
            shards: 1,
            mongos: 1,
            other: {
                keyFile: "jstests/libs/key1",
                mongosOptions: {useLogFiles: true, setParameter: kSetParameter},
                shardOptions: {setParameter: kSetParameter},
            },
        });

        const adminDb = st.s0.getDB("admin");
        adminDb.createUser({user: "admin", pwd: "pwd", roles: ["root"]});
        assert(adminDb.auth("admin", "pwd"), "default connection auth must succeed");
    });

    after(function () {
        st.stop();
    });

    defineAuthTests(
        () => "127.0.0.1:" + st.s0.port,
        () => st.s0,
    );
});
