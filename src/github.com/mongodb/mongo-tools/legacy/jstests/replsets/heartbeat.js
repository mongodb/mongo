// test that we get the proper heartbeat info message on one-way partition

doesEntryMatch = function(array, regex) {
    var found = false;
    for (i = 0; i < array.length; i++) {
        if (regex.test(array[i])) {
            found = true;
        }
    }
    return found;
}

var replTest = new ReplSetTest({name: 'heartbeat', nodes: 3});
var nodes = replTest.nodeList();
replTest.startSet();
replTest.initiate({ "_id": "heartbeat",
                    "members": [
                        {"_id": 0, "host": nodes[0]},
                        {"_id": 1, "host": nodes[1]},
                        {"_id": 2, "host": nodes[2]}]
                  });

// make sure we have a master (getMaster() tries for 60 seconds and throws if master isnt found)
replTest.getMaster();

// start bridges
replTest.bridge();

// check for the message in a lost communication case
replTest.partitionOneWay(1,2);
// check for long enough for the failed communication message to be logged
var msg = RegExp(" just heartbeated us, but our heartbeat failed: ");
assert.soon(function() {
    var log = replTest.nodes[1].getDB("admin").adminCommand({getLog: "global"}).log;
    return doesEntryMatch(log, msg);
}, 25 * 1000, "Did not see an entry about one way heartbeat failure in the log in part 1");

// check for the message in a very delayed communication case
replTest.addOneWayPartitionDelay(0, 1, 15*1000);
// check for long enough for the failed communication message to be logged
assert.soon(function() {
    var log = replTest.nodes[0].getDB("admin").adminCommand({getLog: "global"}).log;
    return doesEntryMatch(log, msg);
}, 25 * 1000, "Did not see an entry about one way heartbeat failure in the log in part 2");
replTest.stopSet();
