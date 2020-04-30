/**
 * Test that commitQuorum option is not supported on standalones for index creation.
 * Note: noPassthrough/commit_quorum.js - Verifies the commitQuorum behavior for replica sets.
 */

(function() {
'use strict';

const standalone = MongoRunner.runMongod();
const db = standalone.getDB("test");

assert.commandWorked(db.createCollection("coll"));

jsTestLog("Create index");
assert.commandFailedWithCode(
    db.runCommand(
        {createIndexes: "coll", indexes: [{name: "x_1", key: {x: 1}}], commitQuorum: "majority"}),
    ErrorCodes.BadValue);

assert.commandFailedWithCode(
    db.runCommand({setIndexCommitQuorum: "coll", indexNames: ["x_1"], commitQuorum: "majority"}),
    ErrorCodes.BadValue);

MongoRunner.stopMongod(standalone);
})();
