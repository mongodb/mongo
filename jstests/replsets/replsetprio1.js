// should check that election happens in priority order

doTest = function( signal ) {

    var replTest = new ReplSetTest( {name: 'testSet', nodes: 3} );
    var nodes = replTest.nodeList();

    replTest.startSet();
    replTest.initiate({"_id" : "testSet",
                "members" : [
                             {"_id" : 0, "host" : nodes[0], "priority" : 1},
                             {"_id" : 1, "host" : nodes[1], "priority" : 2},
                             {"_id" : 2, "host" : nodes[2], "priority" : 3}]});

    // 2 should be master (give this a while to happen, as 0 will be elected, then demoted)
    assert.soon(function() {
        var m2 = replTest.nodes[2].getDB("admin").runCommand({ismaster:1});
        return m2.ismaster;
    }, 'highest priority is master', 120000);

    // kill 2, 1 should take over
    replTest.stop(2);

    // do some writes on 1
    master = replTest.getMaster();
    for (i=0; i<1000; i++) {
        master.getDB("foo").bar.insert({i:i});
    }

    sleep(10000);

    for (i=0; i<1000; i++) {
        master.getDB("bar").baz.insert({i:i});
    }

    var m1 = replTest.nodes[1].getDB("admin").runCommand({ismaster:1})
    assert(m1.ismaster, 'node 2 is master');

    // bring 2 back up, 2 should wait until caught up and then become master
    replTest.restart(2);
    assert.soon(function() {
        try {
            m2 = replTest.nodes[2].getDB("admin").runCommand({ismaster:1})
            return m2.ismaster;
        }
        catch (e) {
            print(e);
        }
        return false;
    }, 'node 2 is master again', 60000);

    // make sure nothing was rolled back
    master = replTest.getMaster();
    for (i=0; i<1000; i++) {
        assert(master.getDB("foo").bar.findOne({i:i}) != null, 'checking '+i);
        assert(master.getDB("bar").baz.findOne({i:i}) != null, 'checking '+i);
    }
}

doTest( 15 );
