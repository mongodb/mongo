/**
 * Tests that creating an index with a partialFilterExpression option generates a startup warning.
 * Also verifies that warnings are generated on startup if a collection has indexes with a
 * partialFilterExpression option.
 */
(function() {
    'use strict';

    var dbpath = MongoRunner.dataPath + 'partial_index_startup_warnings';
    resetDbpath(dbpath);

    // Start a mongod.
    var mongodOptions = {dbpath: dbpath, noCleanData: true};
    var conn = MongoRunner.runMongod(mongodOptions);
    assert.neq(null, conn, 'mongod was unable to start up');
    var testDB = conn.getDB('test');

    // Should have no warnings related to partial indexes initially.
    var warnings = testDB.adminCommand({getLog: 'startupWarnings'});
    assert.eq(0, numWarningsAboutPartialIndexes(warnings),
              'should have had no warnings about partial indexes on fresh startup: ' +
              tojson(warnings));

    // Create an index with a partialFilterExpression option and verify that a warning is logged.
    assert.commandWorked(testDB.coll.createIndex({a: 1}, {partialFilterExpression: {b: {$gt: 10}}}),
                         'failed to create an index with a partialFilterExpression option');

    warnings = testDB.adminCommand({getLog: 'startupWarnings'});
    assert.eq(1, numWarningsAboutPartialIndexes(warnings),
              'should have a warning for the index with a partialFilterExpression: ' +
              tojson(warnings));

    // Create multiple indexes with a partialFilterExpression option and verify that a warning is
    // logged for each.
    var res = testDB.runCommand({
        createIndexes: 'coll',
        indexes: [
            {key: {p: 1}, name: 'p_1', partialFilterExpression: {q: 2}},
            {key: {r: 1}, name: 'r_1'},
            // 't' is technically not a valid filter, but 3.0 shouldn't know any better.
            {key: {s: 1}, name: 's_1', partialFilterExpression: 't'},
        ],
    });
    assert.commandWorked(res,
                         'failed to create multiple indexes with a partialFilterExpression option');

    warnings = testDB.adminCommand({getLog: 'startupWarnings'});
    assert.eq(3, numWarningsAboutPartialIndexes(warnings),
              'should have a warning for each index with a partialFilterExpression: ' +
              tojson(warnings));

    // Shut down the mongod and restart it.
    MongoRunner.stopMongod(conn);

    conn = MongoRunner.runMongod(mongodOptions);
    assert.neq(null, conn, 'mongod was unable to restart');
    testDB = conn.getDB('test');

    // Verify that each index with a partialFilterExpression option generates a startup warning.
    warnings = testDB.adminCommand({getLog: 'startupWarnings'});
    assert.eq(3, numWarningsAboutPartialIndexes(warnings),
              'should have multiple warnings about the other indexes with' +
              ' partialFilterExpression: ' + tojson(warnings));

    MongoRunner.stopMongod(conn);

    function numWarningsAboutPartialIndexes(warnings) {
        var pattern = /partial index/;
        return warnings.log.filter(function(message) {
            return pattern.test(message);
        }).length;
    }
})();
