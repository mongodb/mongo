/**
 * Reproduces the issue where a replacement-style update produces an oplog entry
 * whose 'o' document does not have _id as the first field. The on-disk document
 * is reordered to put _id first, but the oplog entry retains the client's order.
 *
 * @tags: [
 *     requires_fcv_90,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const replTest = new ReplSetTest({nodes: 1, oplogSize: 2});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const coll = primary.getDB("test").otherthings;
const oplog = primary.getDB("local").oplog.rs;

const id = ObjectId();
assert.commandWorked(coll.insert({_id: id, s: "other thing", n: 1}));

assert.commandWorked(coll.update({_id: id}, {s: "other thing", n: 2, _id: id}));

const updateOp = oplog.find({ns: coll.getFullName(), op: "u"}).sort({$natural: -1}).limit(1).next();

jsTest.log.info("Update oplog entry: " + tojson(updateOp));

const firstField = Object.keys(updateOp.o)[0];
jsTest.log.info("First field of oplog 'o' document: " + firstField);

const stored = coll.findOne({_id: id});
const firstStoredField = Object.keys(stored)[0];
jsTest.log.info("First field of stored document: " + firstStoredField);

assert.eq(
    "_id",
    firstField,
    "Expected _id to be the first field in the oplog 'o' document, but got: " + tojson(updateOp.o),
);

replTest.stopSet();
