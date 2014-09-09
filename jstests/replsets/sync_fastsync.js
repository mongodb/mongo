/*
 * Create a replia set where a member has the --fastsync option, 
 * then write some docs and ensure they 
 * are on the newly restarted member (with --fastsync) but older writes aren't.
 */

var testName = "sync_fastsync"
var replTest = new ReplSetTest({name: testName, 
                                nodes: {n0:{}, n1:{}, n2:{fastsync:""}}, 
                                oplogSize: 2});
var nodes = replTest.nodeList();
var conns = replTest.startSet();
var config = { "_id": testName,
               "members": [
                            {"_id": 0, "host": nodes[0]},
                            {"_id": 1, "host": nodes[1]},
                            {"_id": 2, "host": nodes[2], priority:0}]
              };
var r = replTest.initiate(config);
var master = replTest.getMaster();
var mColl = master.getDB("test")[testName];
var sColl = conns[1].getDB("test")[testName];
var fsColl = conns[2].getDB("test")[testName];

mColl.save({_id:1}, {writeConcern:{w:3}});

// Ensure everyone has the same doc, and replication is working normally.
assert.eq({_id:1}, mColl.findOne());
assert.eq({_id:1}, sColl.findOne());
assert.eq({_id:1}, fsColl.findOne());

// Stop fastsync member to test, 3rd node which is prio:0
replTest.stop(2);

// Do write to the other two members, and check it.
mColl.save({_id:2}, {writeConcern:{w:2}});
assert.eq({_id:2}, mColl.findOne({_id:2}));
assert.eq({_id:2}, sColl.findOne({_id:2}));

// Start node without data
replTest.start(2);
assert.soon(function() {
    try{
        return fsColl.getDB().runCommand("isMaster").secondary;
    } catch (e) {
        printjson(e);
        return false;
    }
})

// Make sure only the tail of the oplog is on the fastsync member, plus new write.
mColl.save({_id:3}, {writeConcern:{w:3}});
assert.eq(null, fsColl.findOne({_id:1}), tojson(fsColl.find().toArray()));
assert.eq({_id:2}, fsColl.findOne({_id:2}), tojson(fsColl.find().toArray()));
assert.eq({_id:3}, fsColl.findOne({_id:3}), tojson(fsColl.find().toArray()));

print("****** Test Completed *******")
replTest.stopSet();