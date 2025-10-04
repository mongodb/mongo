/**
 * @tags: [
 *   requires_scripting
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {waitForAllMembers} from "jstests/replsets/rslib.js";

let doTest = function (signal) {
    // Test basic replica set functionality.
    // -- Replication
    // -- Failover

    // Replica set testing API
    // Create a new replica set test. Specify set name and the number of nodes you want.
    let replTest = new ReplSetTest({name: "testSet", nodes: 3});

    // call startSet() to start each mongod in the replica set
    // this returns a list of nodes
    let nodes = replTest.startSet();

    // Call initiate() to send the replSetInitiate command
    // This will wait for initiation
    replTest.initiate();

    // Call getPrimary to return a reference to the node that's been
    // elected primary.
    let primary = replTest.getPrimary();

    // save some records
    let len = 100;
    for (let i = 0; i < len; ++i) {
        primary.getDB("foo").foo.save({a: i});
    }

    waitForAllMembers(primary.getDB("foo"));
    // This method will check the oplogs of the primary
    // and secondaries in the set and wait until the change has replicated.
    replTest.awaitReplication();

    let secondaries = replTest.getSecondaries();
    assert(secondaries.length == 2, "Expected 2 secondaries but length was " + secondaries.length);
    secondaries.forEach(function (secondary) {
        // try to read from secondary
        secondary.setSecondaryOk();
        let count = secondary.getDB("foo").foo.find().itcount();
        printjson(count);
        assert.eq(len, count, "secondary count wrong: " + secondary);

        print("Doing a findOne to verify we can get a row");
        let one = secondary.getDB("foo").foo.findOne();
        printjson(one);

        print("Calling inline mr() with secondaryOk=true, must succeed");
        secondary.setSecondaryOk();
        let map = function () {
            emit(this.a, 1);
        };
        let reduce = function (key, vals) {
            let sum = 0;
            for (let i = 0; i < vals.length; ++i) {
                sum += vals[i];
            }
            return sum;
        };
        secondary.getDB("foo").foo.mapReduce(map, reduce, {out: {"inline": 1}});

        print("Calling mr() to collection with secondaryOk=true, must fail");
        try {
            secondary.getDB("foo").foo.mapReduce(map, reduce, "output");
            assert(false, "mapReduce() to collection succeeded on secondary");
        } catch (e) {
            print("Received exception: " + e);
        }

        print("Calling inline mr() with secondaryOk=false, must fail");
        secondary.setSecondaryOk(false);
        try {
            secondary.getDB("foo").foo.mapReduce(map, reduce, {out: {"inline": 1}});
            assert(false, "mapReduce() succeeded on secondary with secondaryOk=false");
        } catch (e) {
            print("Received exception: " + e);
        }
        print("Calling mr() to collection with secondaryOk=false, must fail");
        try {
            secondary.getDB("foo").foo.mapReduce(map, reduce, "output");
            assert(false, "mapReduce() to collection succeeded on secondary with secondaryOk=false");
        } catch (e) {
            print("Received exception: " + e);
        }
    });

    // Shut down the set and finish the test.
    replTest.stopSet(signal);
};

doTest(15);
print("SUCCESS");
