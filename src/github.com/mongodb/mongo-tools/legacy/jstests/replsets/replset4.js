doTest = function (signal) {

    // Test orphaned master steps down
    var replTest = new ReplSetTest({ name: 'testSet', nodes: 3 });

    replTest.startSet();
    replTest.initiate();

    var master = replTest.getMaster();

    // Kill both slaves, simulating a network partition
    var slaves = replTest.liveNodes.slaves;
    for (var i = 0; i < slaves.length; i++) {
        var slave_id = replTest.getNodeId(slaves[i]);
        replTest.stop(slave_id);
    }

    print("replset4.js 1");

    assert.soon(
        function () {
            try {
                var result = master.getDB("admin").runCommand({ ismaster: 1 });
                return (result['ok'] == 1 && result['ismaster'] == false);
            } catch (e) {
                print("replset4.js caught " + e);
                return false;
            }
        },
        "Master fails to step down when orphaned."
    );

    print("replset4.js worked, stopping");
    replTest.stopSet(signal);
}

print("replset4.js");
doTest( 15 );
print("replset4.js SUCCESS");
