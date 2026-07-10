/**
 * Verifies that clientUpdate validation failures are tracked by the
 * network.clientMetadataUpdate.validationFailures serverStatus metric.
 * @tags: [requires_scripting, requires_sharding]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Raise the global rate limit so back-to-back clientUpdate events in these tests are all logged
// at Info severity. The default (50/sec => 20ms between events) is small enough that consecutive
// subtests on a fast host land in the same bucket and get demoted to Debug(2), making the log
// entry invisible to filters.
const kSetParameter = {clientMetadataUpdateLogRatePerSec: 500};

function getValidationFailedCount(conn) {
    return conn.adminCommand({serverStatus: 1}).metrics.network.clientMetadataUpdate
        .validationFailures;
}

function getClientMetadataUpdateLogCount(conn) {
    return checkLog.getFilteredLogMessages(conn, 51817, {}).length;
}

function defineTests(getConn) {
    it("oversized clientUpdate increments validationFailures", function () {
        const conn = getConn();
        const before = getValidationFailedCount(conn);
        const logCountBefore = getClientMetadataUpdateLogCount(conn);

        const oversizedDoc = {
            driver: {name: "TestDriver", version: "1.0"},
            padding: "x".repeat(1024),
        };
        assert.commandWorked(conn.adminCommand({hello: 1, clientUpdate: oversizedDoc}));

        assert.eq(
            getValidationFailedCount(conn),
            before + 1,
            "validationFailures metric must increment for oversized document",
        );
        assert.eq(
            getClientMetadataUpdateLogCount(conn),
            logCountBefore,
            "clientUpdate log (51817) must not be emitted for oversized document",
        );
    });

    it("non-string cid in clientUpdate increments validationFailures", function () {
        const conn = getConn();
        const before = getValidationFailedCount(conn);
        const logCountBefore = getClientMetadataUpdateLogCount(conn);

        assert.commandWorked(
            conn.adminCommand({
                hello: 1,
                clientUpdate: {cid: 123, driver: {name: "TestDriver", version: "1.0"}},
            }),
        );

        assert.eq(
            getValidationFailedCount(conn),
            before + 1,
            "validationFailures metric must increment for non-string cid",
        );
        assert.eq(
            getClientMetadataUpdateLogCount(conn),
            logCountBefore,
            "clientUpdate log (51817) must not be emitted for non-string cid",
        );
    });
}

describe("clientMetadataUpdate validation metrics on mongod", function () {
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

describe("clientMetadataUpdate validation metrics on mongos", function () {
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
