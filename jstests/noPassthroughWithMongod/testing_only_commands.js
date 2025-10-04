/**
 * Test to make sure that commands and agg stages that should only work when testing commands are
 * enabled via the --enableTestCommands flag fail when that flag isn't provided.
 * @tags: [
 *   disables_test_commands,
 * ]
 */

import {testOnlyCommands} from "jstests/auth/test_only_commands_list.js";

let assertCmdNotFound = function (db, cmdName) {
    let res = db.runCommand(cmdName);
    assert.eq(0, res.ok);
    assert.eq(59, res.code, "expected CommandNotFound(59) error code for test command " + cmdName);
};

let assertCmdFound = function (db, cmdName) {
    let res = db.runCommand(cmdName);
    if (!res.ok) {
        assert.neq(
            59,
            res.code,
            "test command " +
                cmdName +
                " should either have succeeded or " +
                "failed with an error code other than CommandNotFound(59)",
        );
    }
};

const isBoundedSortEnabled = function (conn) {
    const db = conn.getDB("TestDB");
    const coll = db.bounded_sort_coll;
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName()));

    const pipeline = [{$_internalBoundedSort: {sortKey: {t: 1}, bound: {base: "min"}}}];
    const result = db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}});
    if (result.ok) {
        return true;
    }
    // Otherwise it should be disabled with error code 5491300.
    assert.eq(result.code, 5491300);
    return false;
};

// Check if expression $_testApiVersion is only available when enableTestCommands is true
const isTestApiVersionAvailable = function (conn) {
    const db = conn.getDB("TestDB");
    const coll = db.bounded_sort_coll;
    coll.drop();
    db.createCollection(coll.getName());

    const pipeline = [{$project: {v: {$_testApiVersion: {unstable: true}}}}];
    const result = db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}});
    if (result.ok) {
        return true;
    }
    // Otherwise it should be disabled.
    assert.eq(result.code, 31325);
    assert.eq(
        result.errmsg,
        "Invalid $project :: caused by :: Unknown expression $_testApiVersion: This expression is for MongoDB's internal testing only.",
    );
    return false;
};

TestData.enableTestCommands = false;

var conn = MongoRunner.runMongod({});
for (let i in testOnlyCommands) {
    assertCmdNotFound(conn.getDB("test"), testOnlyCommands[i]);
}
assert(!isBoundedSortEnabled(conn));
assert(!isTestApiVersionAvailable(conn));
MongoRunner.stopMongod(conn);

// Now enable the commands
TestData.enableTestCommands = true;

var conn = MongoRunner.runMongod({});
for (let i in testOnlyCommands) {
    assertCmdFound(conn.getDB("test"), testOnlyCommands[i]);
}
assert(isBoundedSortEnabled(conn));
assert(isTestApiVersionAvailable(conn));
MongoRunner.stopMongod(conn);
