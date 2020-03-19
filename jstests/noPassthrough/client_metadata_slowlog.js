/**
 * Test that verifies client metadata is logged as part of slow query logging in MongoD.
 */
load("jstests/libs/logv2_helpers.js");

(function() {
'use strict';

let conn = MongoRunner.runMongod({useLogFiles: true});
assert.neq(null, conn, 'mongod was unable to start up');

let coll = conn.getCollection("test.foo");
assert.commandWorked(coll.insert({_id: 1}));

// Do a really slow query beyond the 100ms threshold
let count = coll.count({
    $where: function() {
        sleep(1000);
        return true;
    }
});
assert.eq(count, 1, "expected 1 document");

print(`Checking ${conn.fullOptions.logFile} for client metadata message`);
let log = cat(conn.fullOptions.logFile);
let predicate = null;
if (isJsonLog(conn)) {
    predicate =
        /Slow query.*test.foo.*"appName":"MongoDB Shell".*"command":{"count":"foo","query":{"\$where":{"\$code":"function\(\)/;
} else {
    predicate =
        /COMMAND .* command test.foo appName: "MongoDB Shell" command: count { count: "foo", query: { \$where: function\(\)/;
}

// Dump the log line by line to avoid log truncation
for (var a of log.split("\n")) {
    print("LOG_FILE_ENTRY: " + a);
}

assert(predicate.test(log),
       "'Slow query' log line missing in mongod log file!\n" +
           "Log file contents: " + conn.fullOptions.logFile);
MongoRunner.stopMongod(conn);
})();
