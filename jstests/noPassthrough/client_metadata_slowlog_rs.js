/**
 * Test that verifies client metadata is logged as part of slow query logging in MongoD in a replica
 * set.
 * @tags: [requires_replication]
 */
(function() {
'use strict';

let checkLog = function(conn) {
    print(`Checking ${conn.fullOptions.logFile} for client metadata message`);
    let log = cat(conn.fullOptions.logFile);
    assert(
        /COMMAND .* command test.foo appName: "MongoDB Shell" command: find { find: "foo", filter: { \$where: function\(\)/
            .test(log),
        "'slow query' log line missing in mongod log file!\n" +
            "Log file contents: " + conn.fullOptions.logFile +
            "\n************************************************************\n" + log +
            "\n************************************************************");
};

let options = {
    useLogFiles: true,
};

const rst = new ReplSetTest({nodes: 2, nodeOptions: options});

rst.startSet();
rst.initiate();
rst.awaitReplication();

// Build a new connection based on the replica set URL
var conn = new Mongo(rst.getURL());

let coll = conn.getCollection("test.foo");
assert.commandWorked(coll.insert({_id: 1}));

// Do a really slow query beyond the 100ms threshold
let count = coll.find({
                    $where: function() {
                        sleep(1000);
                        return true;
                    }
                })
                .readPref('primary')
                .toArray();
checkLog(rst.getPrimary());

// Do a really slow query beyond the 100ms threshold
count = coll.find({
                $where: function() {
                    sleep(1000);
                    return true;
                }
            })
            .readPref('secondary')
            .toArray();
checkLog(rst.getSecondary());

rst.stopSet();
})();
