/**
 * Test that the queryStats HMAC key is not logged.
 * @tags: [requires_fcv_71]
 */

import {getQueryStatsFindCmd} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const checkLogForHmacKey = function(conn) {
    const coll = conn.getCollection("test.foo");
    assert.commandWorked(coll.insert({_id: 1}));
    assert.neq(coll.findOne({_id: 1}), null);

    assert.neq(getQueryStatsFindCmd(conn, {
                   transformIdentifiers: true,
                   hmacKey: BinData(8, "YW4gYXJiaXRyYXJ5IEhNQUNrZXkgZm9yIHRlc3Rpbmc=")
               }),
               null);

    print(`Checking ${conn.fullOptions.logFile} for query stats message`);
    const log = cat(conn.fullOptions.logFile);

    // Make sure there is no unredacted HMAC key
    const predicate = /"hmacKey":"[^#"]+"/;
    assert(!predicate.test(log),
           "Found an unredacted HMAC key in log file!\n" +
               "Log file contents: " + conn.fullOptions.logFile +
               "\n************************************************************\n" + log +
               "\n************************************************************");
};

// Test MongoD
const testMongoD = function() {
    const conn =
        MongoRunner.runMongod({setParameter: {internalQueryStatsRateLimit: -1}, useLogFiles: true});
    assert.neq(null, conn, 'mongod was unable to start up');

    checkLogForHmacKey(conn);

    MongoRunner.stopMongod(conn);
};

// Test MongoS
const testMongoS = function() {
    const options = {
        mongosOptions: {setParameter: {internalQueryStatsRateLimit: -1}, useLogFiles: true},
    };

    const st = new ShardingTest({shards: 1, mongos: 1, other: options});

    checkLogForHmacKey(st.s0);

    st.stop();
};

testMongoD();
testMongoS();
