// Validates cases where index scans are aborted due to the collection being dropped (SERVER-4400)
(function() {
    'use strict';

    var coll = db.jstests_queryoptimizer3;

    var shellWaitHandle = startParallelShell(function() {
        for (var i = 0; i < 400; ++i) {
            sleep(50);
            db.jstests_queryoptimizer3.drop();
        }
    });

    for (var i = 0; i < 100; ++i) {
        coll.drop();
        coll.ensureIndex({a: 1});
        coll.ensureIndex({b: 1});

        for (var j = 0; j < 100; ++j) {
            coll.save({a: j, b: j});
        }

        try {
            var m = i % 5;
            if (m == 0) {
                coll.count({a: {$gte: 0}, b: {$gte: 0}});
            } else if (m == 1) {
                coll.find({a: {$gte: 0}, b: {$gte: 0}}).itcount();
            } else if (m == 2) {
                coll.remove({a: {$gte: 0}, b: {$gte: 0}});
            } else if (m == 3) {
                coll.update({a: {$gte: 0}, b: {$gte: 0}}, {});
            } else if (m == 4) {
                coll.distinct('x', {a: {$gte: 0}, b: {$gte: 0}});
            }
        } catch (e) {
            print("Op killed during yield: " + e.message);
        }
    }

    shellWaitHandle();

    // Ensure that the server is still responding
    assert.commandWorked(db.runCommand({isMaster: 1}));
})();
