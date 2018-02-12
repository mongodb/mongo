// tests getlog as well as slow querying logging
//
// @tags: [
//   # This test attempts to perform a find command and see that it ran using the getLog command.
//   # The former operation may be routed to a secondary in the replica set, whereas the latter must
//   # be routed to the primary.
//   assumes_read_preference_unchanged,
//   does_not_support_stepdowns,
// ]

// We turn off gossiping the mongo shell's clusterTime because it causes the slow command log
// messages to get truncated since they'll exceed 512 characters. The truncated log messages will
// fail to match the find and update patterns defined later on in this test.
TestData.skipGossipingClusterTime = true;

glcol = db.getLogTest2;
glcol.drop();

contains = function(arr, func) {
    var i = arr.length;
    while (i--) {
        if (func(arr[i])) {
            return true;
        }
    }
    return false;
};

// test doesn't work when talking to mongos
if (db.isMaster().msg != "isdbgrid") {
    // run a slow query
    glcol.save({"SENTINEL": 1});
    glcol.findOne({
        "SENTINEL": 1,
        "$where": function() {
            sleep(1000);
            return true;
        }
    });

    // run a slow update
    glcol.update({
        "SENTINEL": 1,
        "$where": function() {
            sleep(1000);
            return true;
        }
    },
                 {"x": "x"});

    var resp = db.adminCommand({getLog: "global"});
    assert(resp.ok == 1, "error executing getLog command");
    assert(resp.log, "no log field");
    assert(resp.log.length > 0, "no log lines");

    // ensure that slow query is logged in detail
    assert(contains(resp.log, function(v) {
        print(v);
        var opString = db.getMongo().useReadCommands() ? " find " : " query ";
        var filterString = db.getMongo().useReadCommands() ? "filter:" : "command:";
        return v.indexOf(opString) != -1 && v.indexOf(filterString) != -1 &&
            v.indexOf("keysExamined:") != -1 && v.indexOf("docsExamined:") != -1 &&
            v.indexOf("SENTINEL") != -1;
    }));

    // same, but for update
    assert(contains(resp.log, function(v) {
        return v.indexOf(" update ") != -1 && v.indexOf("command") != -1 &&
            v.indexOf("keysExamined:") != -1 && v.indexOf("docsExamined:") != -1 &&
            v.indexOf("SENTINEL") != -1;
    }));
}
