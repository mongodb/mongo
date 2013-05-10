// Test for SERVER-8070: Flush buffer before changing sync targets to prevent unnecessary rollbacks
// This test writes 50 ops to one secondary's data (member2) and 25 ops to the other secondary's
// data (member3), then puts 50 more ops in member3's buffer and makes sure that member3 doesn't try
// to sync from member2.

var replSet = new ReplSetTest({name: 'testSet', nodes: 3});
replSet.startSet();
replSet.initiate(
    {
        _id:'testSet',
        members:
        [
            {_id: 0, host: getHostName()+":"+replSet.ports[0]},
            {_id: 1, host: getHostName()+":"+replSet.ports[1], priority: 0},
            {_id: 2, host: getHostName()+":"+replSet.ports[2], priority: 0}
        ]
    }
);

var checkRepl = function(db1, db2) {
    assert.soon(
        function() {
            var last1 = db1.getSisterDB("local").oplog.rs.find().sort({$natural:-1}).limit(1)
                .next();
            var last2 = db2.getSisterDB("local").oplog.rs.find().sort({$natural:-1}).limit(1)
                .next();
            print(tojson(last1)+" "+tojson(last2));

            return ((last1.ts.t === last2.ts.t) && (last1.ts.i === last2.ts.i))
        }
    );
};

// Do an initial write
var master = replSet.getMaster();
master.getDB("foo").bar.insert({x:1});
replSet.awaitReplication();

var primary = master.getDB("foo");
replSet.nodes[1].setSlaveOk();
replSet.nodes[2].setSlaveOk();
var member2 = replSet.nodes[1].getDB("admin");
var member3 = replSet.nodes[2].getDB("admin");

print("Make sure 2 & 3 are syncing from the primary");
member2.adminCommand({replSetSyncFrom : getHostName()+":"+replSet.ports[0]});
member3.adminCommand({replSetSyncFrom : getHostName()+":"+replSet.ports[0]});

print("Stop 2's replication");
member2.runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'});

print("Do a few writes");
for (var i = 0; i < 25; i++) {
    primary.bar.insert({x: i});
}

print("Make sure 3 is at write #25");
checkRepl(primary, member3);
// This means 3's buffer is empty

print("Stop 3's replication");
member3.runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'});

print("Start 2's replication");
member2.runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'off'});

print("Do some writes");
for (var i = 25; i < 50; i++) {
    primary.bar.insert({x: i});
}

print("Make sure 2 is at write #50");
checkRepl(primary, member2);
// This means 2's buffer is empty

print("Stop 2's replication");
member2.runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'});

print("Do some writes - 2 & 3 should have up to write #75 in their buffers, but unapplied");
for (var i = 50; i < 75; i++) {
    primary.bar.insert({x: i});
}
var last = primary.getSisterDB("local").oplog.rs.find().sort({$natural:-1}).limit(1).next();

print("waiting a bit for the secondaries to get the write");
sleep(10000);

print("Shut down the primary");
replSet.stop(0);

// This was used before the server was fixed to prove that the code was broken
var unfixed = function() {
    print("3 should attempt to sync from 2, as 2 is 'ahead'");
    assert.soon(
        function() {
            var syncingTo = member3.adminCommand({replSetGetStatus:1}).syncingTo;
            return syncingTo == getHostName()+":"+replSet.ports[1];
        }
    );
};

var fixed = function() {
    print("3 should not attempt to sync from 2, as it cannot clear its buffer");
    assert.throws(
        function() {
            assert.soon(
                function() {
                    var syncingTo = member3.adminCommand({replSetGetStatus:1}).syncingTo;
                    return syncingTo == getHostName()+":"+replSet.ports[1];
                }
            );
        }
    );
};

//unfixed();
fixed();

print(" --- pause 3's bgsync thread ---");
member3.runCommand({configureFailPoint: 'rsBgSyncProduce', mode: 'alwaysOn'});

print("Allow 3 to apply ops 25-75");
member3.runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'off'});
assert.soon(
    function() {
        var last3 = member3.getSisterDB("local").oplog.rs.find().sort({$natural:-1}).limit(1)
            .next();
        print("primary: " + tojson(last.ts) + " secondary: " + tojson(last3.ts));
        return ((last.ts.t === last3.ts.t) && (last.ts.i === last3.ts.i))
    }
);

print(" --- start 3's bgsync thread ---");
member3.runCommand({configureFailPoint: 'rsBgSyncProduce', mode: 'off'});

print("Shouldn't hit rollback");
var end = (new Date()).getTime()+10000;
while ((new Date()).getTime() < end) {
    assert('ROLLBACK' != member3.runCommand({replSetGetStatus:1}).members[2].stateStr);
    sleep(30);
}

replSet.stopSet();
