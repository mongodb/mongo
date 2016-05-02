/*
 * Test which configures various configs (hidden/priorities/no-chaining) that replExec queues
 * stay at reasonable/stable levels after repeated reconfigs/stepdowns
 */
(function() {
    "use strict";
    var numNodes = 5;
    var maxQueueSizeExpected = 11;
    var replTest = new ReplSetTest({name: 'testSet', nodes: numNodes});
    var nodes = replTest.startSet();
    replTest.initiate();

    var primary = replTest.getPrimary();

    var testQueues = function() {
        /* Example stats under executor
                "counters" : {
                        "eventCreated" : 2,
                        "eventWait" : 2,
                        "cancels" : 17,
                        "waits" : 490,
                        "scheduledNetCmd" : 90,
                        "scheduledDBWork" : 2,
                        "scheduledXclWork" : 0,
                        "scheduledWorkAt" : 120,
                        "scheduledWork" : 494,
                        "schedulingFailures" : 0
                },
                "queues" : {
                        "networkInProgress" : 0,
                        "dbWorkInProgress" : 0,
                        "exclusiveInProgress" : 0,
                        "sleepers" : 3,
                        "ready" : 0,
                        "free" : 4
                },

        */
        assert.soon(function() {
            primary = replTest.getPrimary();
            try {
                var stats = replTest.nodes.map(m => m.getDB("admin").serverStatus());
                stats.forEach(s => {
                    var executorStats = s.metrics.repl.executor;
                    printjson(s.host);
                    printjson(executorStats);
                    var queues = executorStats.queues;
                    assert.lt(queues.sleepers, maxQueueSizeExpected, "sleepers");
                    assert.lt(queues.ready, maxQueueSizeExpected, "ready");
                    assert.lt(queues.networkInProgress, maxQueueSizeExpected, "networkInProgress");
                });
            } catch (e) {
                return false;
            }
            return true;
        }, "queues too high", 13 * 1000 /*13 secs*/);  // what we are looking for has a 10s timeout.
    };

    var reconfig = function(newConfig) {
        newConfig.version += 1;
        try {
            assert.commandWorked(replTest.getPrimary().adminCommand({replSetReconfig: newConfig}));
        } catch (e) {
            if (tojson(e).indexOf("error doing query: failed") < 0) {
                throw e;
            }
        }
    };

    replTest.awaitSecondaryNodes();

    // ** Setup different priorities
    var c = replTest.getReplSetConfigFromNode();
    c.members[0].priority = 99;
    c.members[1].priority = 2;
    c.members[2].priority = 0;
    reconfig(c);

    for (var i = 0; i < 50; i++) {
        reconfig(c);
        testQueues();
    }

    // ** Setup different priorities
    var c = replTest.getReplSetConfigFromNode();
    c.members[2].hidden = true;
    c.members[3].priority = 1000;
    c.members[4].priority = 1000;
    reconfig(c);

    for (var i = 0; i < 50; i++) {
        reconfig(c);
        testQueues();
    }

    replTest.stopSet();
}());
