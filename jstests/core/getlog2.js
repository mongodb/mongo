// tests getlog as well as slow querying logging
//
// @tags: [
//   # This test attempts to perform a find command and see that it ran using the getLog command.
//   # The former operation may be routed to a secondary in the replica set, whereas the latter must
//   # be routed to the primary.
//   assumes_read_preference_unchanged,
//   does_not_support_stepdowns,
// ]

(function() {
    'use strict';

    // We turn off gossiping the mongo shell's clusterTime because it causes the slow command log
    // messages to get truncated since they'll exceed 512 characters. The truncated log messages
    // will fail to match the find and update patterns defined later on in this test.
    TestData.skipGossipingClusterTime = true;

    const glcol = db.getLogTest2;
    glcol.drop();

    function contains(arr, func) {
        let i = arr.length;
        while (i--) {
            if (func(arr[i])) {
                return true;
            }
        }
        return false;
    }

    // test doesn't work when talking to mongos
    if (db.isMaster().msg === "isdbgrid") {
        return;
    }

    // 1. Run a slow query
    glcol.save({"SENTINEL": 1});
    glcol.findOne({
        "SENTINEL": 1,
        "$where": function() {
            sleep(1000);
            return true;
        }
    });

    const query = assert.commandWorked(db.adminCommand({getLog: "global"}));
    assert(query.log, "no log field");
    assert.gt(query.log.length, 0, "no log lines");

    // Ensure that slow query is logged in detail.
    assert(contains(query.log, function(v) {
        print(v);
        const opString = db.getMongo().useReadCommands() ? " find " : " query ";
        const filterString = db.getMongo().useReadCommands() ? "filter:" : "command:";
        return v.indexOf(opString) != -1 && v.indexOf(filterString) != -1 &&
            v.indexOf("keysExamined:") != -1 && v.indexOf("docsExamined:") != -1 &&
            v.indexOf("SENTINEL") != -1;
    }));

    // 2. Run a slow update
    glcol.update({
        "SENTINEL": 1,
        "$where": function() {
            sleep(1000);
            return true;
        }
    },
                 {"x": "x"});

    const update = assert.commandWorked(db.adminCommand({getLog: "global"}));
    assert(update.log, "no log field");
    assert.gt(update.log.length, 0, "no log lines");

    // Ensure that slow update is logged in deail.
    assert(contains(update.log, function(v) {
        print(v);
        return v.indexOf(" update ") != -1 && v.indexOf("command") != -1 &&
            v.indexOf("keysExamined:") != -1 && v.indexOf("docsExamined:") != -1 &&
            v.indexOf("SENTINEL") != -1;
    }));
})();
