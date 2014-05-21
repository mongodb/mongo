// test that a rollback of collModding usePowerOf2Sizes will cause the node to log a message about
// ignoring that action during rollback

// function to check the logs for an entry
doesEntryMatch = function(array, regex) {
    var found = false;
    for (i = 0; i < array.length; i++) {
        if (regex.test(array[i])) {
            found = true;
        }
    }
    return found;
}

// set up a set and grab things for later
var name = "rollback_collMod_PowerOf2Sizes";
var replTest = new ReplSetTest({name: name, nodes: 3});
var nodes = replTest.nodeList();
var conns = replTest.startSet();
replTest.initiate({"_id": name,
                   "members": [
                       { "_id": 0, "host": nodes[0], priority: 3 },
                       { "_id": 1, "host": nodes[1] },
                       { "_id": 2, "host": nodes[2], arbiterOnly: true}]
                  });
var a_conn = conns[0];
var b_conn = conns[1];
var AID = replTest.getNodeId(a_conn);
var BID = replTest.getNodeId(b_conn);

// get master and do an initial write
var master = replTest.getMaster();
assert(master === conns[0], "conns[0] assumed to be master");
assert(a_conn.host === master.host, "a_conn assumed to be master");
var options = {writeConcern: {w: 2, wtimeout: 60000}, upsert: true};
assert.writeOK(a_conn.getDB(name).foo.insert({x: 1}, options));

// shut down master
replTest.stop(AID);

// collMod usePowerOf2Sizes to be false
master = replTest.getMaster();
assert(b_conn.host === master.host, "b_conn assumed to be master");
assert.commandWorked(b_conn.getDB(name).runCommand({collMod: "foo", usePowerOf2Sizes: false}));

// shut down B and bring back the original master
replTest.stop(BID);
replTest.restart(AID);
master = replTest.getMaster();
assert(a_conn.host === master.host, "a_conn assumed to be master");

// do a write so that B will have to roll back
options = {writeConcern: {w: 1, wtimeout: 60000}, upsert: true};
assert.writeOK(a_conn.getDB(name).foo.insert({x: 2}, options));

// restart B, which should rollback and log a message about not rolling back usePowerOf2Sizes
replTest.restart(BID);
var msg = RegExp("replSet not rolling back change of usePowerOf2Sizes: ");
assert.soon(function() {
    try {
        var log = b_conn.getDB("admin").adminCommand({getLog: "global"}).log;
        return doesEntryMatch(log, msg);
    }
    catch (e) {
        return false;
    }
}, "Did not see a log entry about skipping the usePowerOf2Sizes command during rollback");

replTest.stopSet();
