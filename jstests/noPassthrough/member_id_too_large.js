// Tests replSetInitiate and replSetReconfig with member _id values greater than the number of
// members in the set, followed by waiting for writeConcern with "w" values equal to size of set.
// @tags: [requires_replication]
(function() {
"use strict";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();

jsTestLog("replSetInitiate with member _id greater than number of members");

let conf = rst.getReplSetConfig();
conf.members[1]._id = 2;

rst.initiate(conf);

const dbName = "test";
const collName = "test";
const primary = rst.getPrimary();
const testColl = primary.getDB(dbName).getCollection(collName);
const doc = {
    a: 1
};

assert.commandWorked(testColl.insert(doc, {writeConcern: {w: 2}}));

jsTestLog("replSetReconfig with member _id greater than number of members");

let secondary2 = MongoRunner.runMongod({replSet: rst.name});
conf = rst.getReplSetConfigFromNode();
conf.version++;
conf.members.push({_id: 5, host: secondary2.host});
assert.commandWorked(primary.getDB("admin").runCommand({replSetReconfig: conf}));
assert.commandWorked(testColl.insert(doc, {writeConcern: {w: 2}}));
assert.commandWorked(testColl.insert(doc, {writeConcern: {w: 3}}));

MongoRunner.stopMongod(secondary2);
rst.stopSet();
})();
