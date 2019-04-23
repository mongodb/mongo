/**
 * Make sure that a speculative majority change stream read on a secondary does not trigger an
 * invariant when there are conflicting catalog changes on the collection.
 *
 * Regression test for SERVER-40706.
 *
 *  @tags: [uses_speculative_majority]
 */
(function() {
    "use strict";

    const replTest = new ReplSetTest({
        name: "replset",
        nodes: [{}, {rsConfig: {priority: 0}}],
        nodeOptions: {enableMajorityReadConcern: 'false'}
    });
    replTest.startSet();
    replTest.initiate();

    const dbName = "test";
    const collName = "coll";

    let primary = replTest.getPrimary();
    let secondary = replTest.getSecondary();
    let primaryDB = primary.getDB(dbName);
    let primaryColl = primaryDB[collName];
    let secondaryDB = secondary.getDB(dbName);

    // Insert some documents on the primary that we can index.
    var bulk = primaryColl.initializeUnorderedBulkOp();
    for (var i = 0; i < 1000; i++) {
        let doc = {};
        bulk.insert({a: i, b: i, c: i, d: i, e: i});
    }
    assert.commandWorked(bulk.execute());

    // Start several index builds on the primary. This should make it likely that index builds are
    // in progress on the secondary while doing reads below.
    primaryColl.createIndex({a: 1});
    primaryColl.createIndex({b: 1});
    primaryColl.createIndex({c: 1});
    primaryColl.createIndex({d: 1});
    primaryColl.createIndex({e: 1});

    // Do a bunch of change stream reads against the secondary. We are not worried about the
    // responses, since we are only verifying that the server doesn't crash.
    for (var i = 0; i < 20; i++) {
        assert.commandWorked(secondaryDB.runCommand(
            {aggregate: collName, pipeline: [{$changeStream: {}}], cursor: {}}));
    }

    replTest.stopSet();
})();