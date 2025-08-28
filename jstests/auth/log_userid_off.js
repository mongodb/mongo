/**
 * Tests that logged users will not show up in the log.
 *
 * @param mongo {Mongo} connection object.
 * @tags: [requires_sharding]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

let doTest = function (mongo, callSetParam) {
    let TEST_USER = "foo";
    let TEST_PWD = "bar";
    let testDB = mongo.getDB("test");

    testDB.createUser({user: TEST_USER, pwd: TEST_PWD, roles: jsTest.basicUserRoles});
    testDB.auth(TEST_USER, TEST_PWD);

    testDB.runCommand({dbStats: 1});

    let log = testDB.adminCommand({getLog: "global"});
    log.log.forEach(function (line) {
        assert.eq(-1, line.indexOf("user: foo@"), "user logged: " + line);
    });

    // logUserIds should not be settable
    let res = testDB.runCommand({setParameter: 1, logUserIds: 1});
    assert(!res.ok);

    testDB.runCommand({dbStats: 1});

    log = testDB.adminCommand({getLog: "global"});
    log.log.forEach(function (line) {
        assert.eq(-1, line.indexOf("user: foo@"), "user logged: " + line);
    });
};

let mongo = MongoRunner.runMongod({verbose: 5});
doTest(mongo);
MongoRunner.stopMongod(mongo);

let st = new ShardingTest({shards: 1, verbose: 5});
doTest(st.s);
st.stop();
