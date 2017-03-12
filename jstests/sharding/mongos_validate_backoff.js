// Ensures that single mongos shard-key errors are fast, but slow down when many are triggered
(function() {
    'use strict';

    var st = new ShardingTest({shards: 1, mongos: 1});

    var mongos = st.s0;
    var admin = mongos.getDB("admin");
    var coll = mongos.getCollection("foo.bar");

    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));

    coll.ensureIndex({shardKey: 1});
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {shardKey: 1}}));

    var timeBadInsert = function() {
        var start = new Date().getTime();

        // Bad insert, no shard key
        assert.writeError(coll.insert({hello: "world"}));

        var end = new Date().getTime();

        return end - start;
    };

    // We need to work at least twice in order to check resetting the counter
    var successNeeded = 2;
    var success = 0;

    // Loop over this test a few times, to ensure that the error counters get reset if we don't have
    // bad inserts over a long enough time.
    for (var test = 0; test < 5; test++) {
        var firstWait = timeBadInsert();
        var lastWait = 0;

        for (var i = 0; i < 20; i++) {
            printjson(lastWait = timeBadInsert());
        }

        // As a heuristic test, we want to make sure that the error wait after sleeping is much less
        // than the error wait after a lot of errors.
        if (lastWait > firstWait * 2 * 2) {
            success++;
        }

        if (success >= successNeeded) {
            break;
        }

        // Abort if we've failed too many times
        assert.lt(test, 4);

        // Sleeping for long enough to reset our exponential counter
        sleep(3000);
    }

    st.stop();

})();
