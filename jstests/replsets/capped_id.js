// Check that capped collections get an _id index when replicated

// Test #
// 0) create capped collection on replset, check _id index appears on secondary
// 1) create normal collection, then convertToCapped, check _id index on secondaries
// 2) create capped collection, do updates instead of inserts, check _id index on secondaries
// 3) create capped collection with autoIndexId=false. make sure no _id index. then create one
//    and check it got created on secondaries.

// Create a new replica set test with name 'testSet' and 3 members
var replTest = new ReplSetTest({name: 'testSet', nodes: 3});

// call startSet() to start each mongod in the replica set
// this returns a list of nodes
var nodes = replTest.startSet();

// Call initiate() to send the replSetInitiate command
// This will wait for initiation
replTest.initiate();

// Call getPrimary to return a reference to the node that's been
// elected master
var master = replTest.getPrimary();

// wait for secondaries to be up, since we'll be reading from them
replTest.awaitSecondaryNodes();
// And get the slaves from the liveNodes
var slave1 = replTest.liveNodes.slaves[0];
var slave2 = replTest.liveNodes.slaves[1];

// Calling getPrimary made available the liveNodes structure,
// which looks like this:
// liveNodes = {master: masterNode, slaves: [slave1, slave2] }
printjson(replTest.liveNodes);

// define db names to use for this test
var dbname = "dbname";
var masterdb = master.getDB(dbname);
var slave1db = slave1.getDB(dbname);
var slave2db = slave2.getDB(dbname);

function countIdIndexes(theDB, coll) {
    return theDB[coll]
        .getIndexes()
        .filter(function(idx) {
            return friendlyEqual(idx.key, {_id: 1});
        })
        .length;
}

var numtests = 4;
for (testnum = 0; testnum < numtests; testnum++) {
    // define collection name
    coll = "coll" + testnum;

    // drop the coll on the master (just in case it already existed)
    // and wait for the drop to replicate
    masterdb.getCollection(coll).drop();
    replTest.awaitReplication();

    if (testnum == 0) {
        // create a capped collection on the master
        // insert a bunch of things in it
        // wait for it to replicate
        masterdb.runCommand({create: coll, capped: true, size: 1024});
        for (i = 0; i < 500; i++) {
            masterdb.getCollection(coll).insert({a: 1000});
        }
        replTest.awaitReplication();
    } else if (testnum == 1) {
        // create a non-capped collection on the master
        // insert a bunch of things in it
        // wait for it to replicate
        masterdb.runCommand({create: coll});
        for (i = 0; i < 500; i++) {
            masterdb.getCollection(coll).insert({a: 1000});
        }
        replTest.awaitReplication();

        // make sure _id index exists on primary
        assert.eq(1,
                  countIdIndexes(masterdb, coll),
                  "master does not have _id index on normal collection");

        // then convert it to capped
        masterdb.runCommand({convertToCapped: coll, size: 1024});
        replTest.awaitReplication();
    } else if (testnum == 2) {
        // similar to first test, but check that a bunch of updates instead
        // of inserts triggers the _id index creation on secondaries.
        masterdb.runCommand({create: coll, capped: true, size: 1024});
        masterdb.getCollection(coll).insert({a: 0});
        for (i = 0; i < 500; i++) {
            masterdb.getCollection(coll).update({}, {$inc: {a: 1}});
        }
        replTest.awaitReplication();
    } else if (testnum == 3) {
        // explicitly set autoIndexId : false
        masterdb.runCommand({create: coll, capped: true, size: 1024, autoIndexId: false});
        for (i = 0; i < 500; i++) {
            masterdb.getCollection(coll).insert({a: 1000});
        }
        replTest.awaitReplication();

        assert.eq(0,
                  countIdIndexes(masterdb, coll),
                  "master has an _id index on capped collection when autoIndexId is false");
        assert.eq(0,
                  countIdIndexes(slave1db, coll),
                  "slave1 has an _id index on capped collection when autoIndexId is false");
        assert.eq(0,
                  countIdIndexes(slave2db, coll),
                  "slave2 has an _id index on capped collection when autoIndexId is false");

        // now create the index and make sure it works
        masterdb.getCollection(coll).ensureIndex({"_id": 1});
        replTest.awaitReplication();
    }

    // what indexes do we have?
    print("**********Master indexes on " + dbname + "." + coll + ":**********");
    masterdb[coll].getIndexes().forEach(printjson);
    print("");

    print("**********Slave1 indexes on " + dbname + "." + coll + ":**********");
    slave1db[coll].getIndexes().forEach(printjson);
    print("");

    print("**********Slave2 indexes on " + dbname + "." + coll + ":**********");
    slave2db[coll].getIndexes().forEach(printjson);
    print("");

    // ensure all nodes have _id index
    assert.eq(1, countIdIndexes(masterdb, coll), "master has an _id index on capped collection");
    assert.eq(
        1, countIdIndexes(slave1db, coll), "slave1 does not have _id index on capped collection");
    assert.eq(
        1, countIdIndexes(slave2db, coll), "slave2 does not have _id index on capped collection");

    print("capped_id.js Test # " + testnum + " SUCCESS");
}

// Finally, stop set
replTest.stopSet();
