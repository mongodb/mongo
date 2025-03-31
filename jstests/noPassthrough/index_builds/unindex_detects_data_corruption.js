/**
 * This tests that errors are logged when unindexing _id finds evidence of corruption, the server
 * does not crash, and the appropriate error is returned.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const replSet = new ReplSetTest({nodes: 1});
replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();

const db = primary.getDB('test');
const collName = 'coll';
const coll = db[collName];

assert.commandWorked(coll.insert({a: "first"}));

assert.commandWorked(primary.adminCommand(
    {configureFailPoint: "WTIndexUassertDuplicateRecordForKeyOnIdUnindex", mode: "alwaysOn"}));

assert.commandFailedWithCode(coll.remove({a: "first"}), ErrorCodes.DataCorruptionDetected);

assert.commandWorked(primary.adminCommand(
    {configureFailPoint: "WTIndexUassertDuplicateRecordForKeyOnIdUnindex", mode: "off"}));

replSet.stopSet();