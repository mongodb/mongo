/**
 * Tests that replSetInitiate and replSetReconfig ignore hostname case.
 *
 * @tags: [requires_replication]
 */

(function() {
    "use strict";

    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();

    jsTestLog("replSetInitiate with uppercase hostname");

    let conf = rst.getReplSetConfig();
    conf.members[0].host = conf.members[0].host.toUpperCase();

    rst.initiate(conf);

    const dbName = "test";
    const collName = "test";
    const primary = rst.getPrimary();
    const testColl = primary.getDB(dbName).getCollection(collName);
    const doc = {a: 1};

    assert.commandWorked(testColl.insert(doc, {writeConcern: {w: 2}}));

    jsTestLog("replSetReconfig with uppercase hostname");

    let secondary2 = MongoRunner.runMongod({replSet: rst.name});
    conf = rst.getReplSetConfigFromNode();
    conf.version++;
    conf.members.push({_id: 5, host: secondary2.host.toUpperCase()});
    assert.commandWorked(primary.getDB("admin").runCommand({replSetReconfig: conf}));
    assert.commandWorked(testColl.insert(doc, {writeConcern: {w: 2}}));
    assert.commandWorked(testColl.insert(doc, {writeConcern: {w: 3}}));

    MongoRunner.stopMongod(secondary2);
    rst.stopSet();
})();
