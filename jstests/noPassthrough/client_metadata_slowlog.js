/**
 * Test that verifies client metadata is logged as part of slow query logging in MongoD.
 */
(function() {
    'use strict';

    let conn = MongoRunner.runMongod({useLogFiles: true});
    assert.neq(null, conn, 'merizod was unable to start up');

    let coll = conn.getCollection("test.foo");
    assert.writeOK(coll.insert({_id: 1}));

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
    assert(
        /COMMAND .* command test.foo appName: "MerizoDB Shell" command: count { count: "foo", query: { \$where: function\(\)/
            .test(log),
        "'slow query' log line missing in merizod log file!\n" + "Log file contents: " +
            conn.fullOptions.logFile +
            "\n************************************************************\n" + log +
            "\n************************************************************");

    MongoRunner.stopMongod(conn);
})();
