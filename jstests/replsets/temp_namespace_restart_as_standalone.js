/**
 * Tests that temporary collections are not dropped when a member of a replica set is started up as
 * a stand-alone mongod, i.e. without the --replSet parameter.
 *
 * @tags: [requires_persistence]
 */
(function() {
    var rst = new ReplSetTest({nodes: 2});
    rst.startSet();

    // Rig the election so that the first node becomes the primary and remains primary despite the
    // secondary being terminated during this test.
    var replSetConfig = rst.getReplSetConfig();
    replSetConfig.members[1].priority = 0;
    replSetConfig.members[1].votes = 0;
    rst.initiate(replSetConfig);

    var primaryConn = rst.getPrimary();
    var secondaryConn = rst.getSecondary();

    var primaryDB = primaryConn.getDB("test");
    var secondaryDB = secondaryConn.getDB("test");

    // Create a temporary collection and wait until the operation has replicated to the secondary.
    assert.commandWorked(primaryDB.runCommand({
        create: "temp_collection",
        temp: true,
        writeConcern: {w: 2, wtimeout: 60000},
    }));

    // Verify that the temporary collection exists on the primary and has temp=true.
    var primaryCollectionInfos = primaryDB.getCollectionInfos({name: "temp_collection"});
    assert.eq(1, primaryCollectionInfos.length, "'temp_collection' wasn't created on the primary");
    assert.eq("temp_collection",
              primaryCollectionInfos[0].name,
              "'temp_collection' wasn't created on the primary");
    assert.eq(true,
              primaryCollectionInfos[0].options.temp,
              "'temp_collection' wasn't created as temporary on the primary: " +
                  tojson(primaryCollectionInfos[0].options));

    // Verify that the temporary collection exists on the secondary and has temp=true.
    var secondaryCollectionInfos = secondaryDB.getCollectionInfos({name: "temp_collection"});
    assert.eq(
        1, secondaryCollectionInfos.length, "'temp_collection' wasn't created on the secondary");
    assert.eq("temp_collection",
              secondaryCollectionInfos[0].name,
              "'temp_collection' wasn't created on the secondary");
    assert.eq(true,
              secondaryCollectionInfos[0].options.temp,
              "'temp_collection' wasn't created as temporary on the secondary: " +
                  tojson(secondaryCollectionInfos[0].options));

    // Shut down the secondary and restart it as a stand-alone mongod.
    var secondaryNodeId = rst.getNodeId(secondaryDB.getMongo());
    rst.stop(secondaryNodeId);

    secondaryConn = MongoRunner.runMongod({dbpath: secondaryConn.dbpath, noCleanData: true});
    assert.neq(null, secondaryConn, "secondary failed to start up as a stand-alone mongod");
    secondaryDB = secondaryConn.getDB("test");

    // Verify that the temporary collection still exists on the secondary and has temp=true.
    secondaryCollectionInfos = secondaryDB.getCollectionInfos({name: "temp_collection"});
    assert.eq(1,
              secondaryCollectionInfos.length,
              "'temp_collection' was dropped after restarting the secondary as a stand-alone");
    assert.eq("temp_collection",
              secondaryCollectionInfos[0].name,
              "'temp_collection' was dropped after restarting the secondary as a stand-alone");
    assert.eq(true,
              secondaryCollectionInfos[0].options.temp,
              "'temp_collection' is no longer temporary after restarting the secondary as a" +
                  " stand-alone: " + tojson(secondaryCollectionInfos[0].options));

    // Shut down the secondary and restart it as a member of the replica set.
    MongoRunner.stopMongod(secondaryConn);

    var restart = true;
    rst.start(secondaryNodeId, {}, restart);

    // Verify that writes are replicated to the temporary collection and can successfully be applied
    // by the secondary after having restarted it.
    assert.writeOK(primaryDB.temp_collection.insert({}, {writeConcern: {w: 2, wtimeout: 60000}}));

    rst.stopSet();
})();
