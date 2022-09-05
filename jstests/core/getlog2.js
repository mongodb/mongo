// tests getlog as well as slow querying logging
//
// @tags: [
//   # This test attempts to perform a find command and see that it ran using the getLog command.
//   # The former operation may be routed to a secondary in the replica set, whereas the latter must
//   # be routed to the primary.
//   assumes_read_preference_unchanged,
//   does_not_support_stepdowns,
//   no_selinux,
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

function stringContains(haystack, needle) {
    if (needle.indexOf(":"))
        needle = '"' + needle.replace(':', "\":");
    needle = needle.replace(/ /g, "");
    return haystack.indexOf(needle) != -1;
}

// test doesn't work when talking to mongos
if (db.hello().msg === "isdbgrid") {
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
    return stringContains(v, " find ") && stringContains(v, "filter:") &&
        stringContains(v, "keysExamined:") && stringContains(v, "docsExamined:") &&
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
    return stringContains(v, " update ") != -1 && stringContains(v, "command") &&
        stringContains(v, "keysExamined:") && stringContains(v, "docsExamined:") &&
        v.indexOf("SENTINEL") != -1;
}));
})();
