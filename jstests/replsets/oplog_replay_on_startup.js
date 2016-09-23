// SERVER-7200 On startup, replica set nodes delete oplog state past the oplog delete point and
// apply any remaining unapplied ops before coming up as a secondary.
//
// @tags: [requires_persistence]
(function() {
    "use strict";

    var ns = "test.coll";

    var rst = new ReplSetTest({
        nodes: 1,
    });

    rst.startSet();
    rst.initiate();

    var conn = rst.getPrimary();  // Waits for PRIMARY state.
    var nojournal = Array.contains(conn.adminCommand({getCmdLineOpts: 1}).argv, '--nojournal');
    var storageEngine = jsTest.options().storageEngine;
    var term = conn.getCollection('local.oplog.rs').find().sort({$natural: -1}).limit(1).next().t;
    if (typeof(term) == 'undefined') {
        term = -1;  // Use a dummy term for PV0.
    }

    function runTest({
        oplogEntries,
        collectionContents,
        deletePoint,
        begin,
        minValid,
        expectedState,
        expectedApplied,
    }) {
        if (nojournal && (storageEngine === 'mmapv1') && expectedState === 'FATAL') {
            // We can't test fatal states on mmap without a journal because it won't be able
            // to start up again.
            return;
        }

        if (term != -1) {
            term++;  // Each test gets a new term on PV1 to ensure OpTimes always move forward.
        }

        conn = rst.restart(0, {noReplSet: true});  // Restart as a standalone node.
        assert.neq(null, conn, "failed to restart");
        var oplog = conn.getCollection('local.oplog.rs');
        var minValidColl = conn.getCollection('local.replset.minvalid');
        var coll = conn.getCollection(ns);

        // Reset state to empty.
        assert.commandWorked(oplog.runCommand('emptycapped'));
        coll.drop();
        assert.commandWorked(coll.runCommand('create'));

        var ts = (num) => num === null ? Timestamp() : Timestamp(1000, num);

        oplogEntries.forEach((num) => {
            assert.writeOK(oplog.insert({
                ts: ts(num),
                t: term,
                h: 1,
                op: 'i',
                ns: ns,
                o: {_id: num},
            }));
        });

        collectionContents.forEach((num) => {
            assert.writeOK(coll.insert({_id: num}));
        });

        var injectedMinValidDoc = {
            _id: ObjectId(),

            // minvalid:
            ts: ts(minValid),
            t: term,

            // appliedThrough
            begin: {
                ts: ts(begin),
                t: term,
            },

            oplogDeleteFromPoint: ts(deletePoint),
        };

        // This weird mechanism is the only way to bypass mongod's attempt to fill in null
        // Timestamps.
        assert.writeOK(minValidColl.remove({}));
        assert.writeOK(minValidColl.update({}, {$set: injectedMinValidDoc}, {upsert: true}));
        assert.eq(minValidColl.findOne(),
                  injectedMinValidDoc,
                  "If the Timestamps differ, the server may be filling in the null timestamps");

        try {
            conn = rst.restart(0);  // Restart in replSet mode again.
        } catch (e) {
            assert.eq(expectedState, 'FATAL', 'node failed to restart: ' + e);
            return;
        }

        // Wait for the node to go to SECONDARY if it is able.
        assert.soon(
            () =>
                conn.adminCommand('serverStatus').metrics.repl.apply.attemptsToBecomeSecondary > 0,
            () => conn.adminCommand('serverStatus').metrics.repl.apply.attemptsToBecomeSecondary);

        var isMaster = conn.adminCommand('ismaster');
        switch (expectedState) {
            case 'SECONDARY':
                // Primary is also acceptable since once a node becomes secondary, it will try to
                // become primary if it is eligible and has enough votes (which this node does).
                // This is supposed to test that we reach secondary, not that we stay there.
                assert(isMaster.ismaster || isMaster.secondary,
                       'not PRIMARY or SECONDARY: ' + tojson(isMaster));
                break;

            case 'RECOVERING':
                assert(!isMaster.ismaster && !isMaster.secondary,
                       'not in RECOVERING: ' + tojson(isMaster));

                // Restart as a standalone node again so we can read from the collection.
                conn = rst.restart(0, {noReplSet: true});
                break;

            case 'FATAL':
                doassert("server startup didn't fail when it should have");
                break;

            default:
                doassert(`expectedState ${expectedState} is not supported`);
        }

        // Ensure the oplog has the entries it should have and none that it shouldn't.
        assert.eq(conn.getCollection('local.oplog.rs')
                      .find({ns: ns, op: 'i'})
                      .sort({$natural: 1})
                      .map((op) => op.o._id),
                  expectedApplied);

        // Ensure that all ops that should have been applied were.
        conn.setSlaveOk(true);
        assert.eq(conn.getCollection(ns).find().sort({_id: 1}).map((obj) => obj._id),
                  expectedApplied);
    }

    //
    // Normal 3.4 cases
    //

    runTest({
        oplogEntries: [1, 2, 3],
        collectionContents: [1, 2, 3],
        deletePoint: null,
        begin: null,
        minValid: null,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3],
    });

    runTest({
        oplogEntries: [1, 2, 3],
        collectionContents: [1, 2, 3],
        deletePoint: null,
        begin: null,
        minValid: 2,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3],
    });

    runTest({
        oplogEntries: [1, 2, 3],
        collectionContents: [1, 2, 3],
        deletePoint: null,
        begin: null,
        minValid: 3,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3],
    });

    runTest({
        oplogEntries: [1, 2, 3],
        collectionContents: [1, 2, 3],
        deletePoint: null,
        begin: 3,
        minValid: 3,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3],
    });

    runTest({
        oplogEntries: [1, 2, 3],
        collectionContents: [1, 2, 3],
        deletePoint: 4,
        begin: 3,
        minValid: 3,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3],
    });

    runTest({
        oplogEntries: [1, 2, 3, 4, 5, 6],
        collectionContents: [1, 2, 3],
        deletePoint: 4,
        begin: 3,
        minValid: 3,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3],
    });

    runTest({
        oplogEntries: [1, 2, 3, /*4,*/ 5, 6],
        collectionContents: [1, 2, 3],
        deletePoint: 4,
        begin: 3,
        minValid: 3,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3],
    });

    runTest({
        oplogEntries: [1, 2, 3, 4, 5, 6],
        collectionContents: [1, 2, 3],
        deletePoint: null,
        begin: 3,
        minValid: 3,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3, 4, 5, 6],
    });

    runTest({
        oplogEntries: [1, 2, 3, 4, 5, 6],
        collectionContents: [1, 2, 3],
        deletePoint: null,
        begin: 3,
        minValid: 6,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3, 4, 5, 6],
    });

    //
    // 3.2 -> 3.4 upgrade cases
    //

    runTest({
        oplogEntries: [1, 2, 3],
        collectionContents: [1, 2, 3],
        deletePoint: null,
        begin: 3,
        minValid: 6,
        expectedState: 'RECOVERING',
        expectedApplied: [1, 2, 3],
    });

    runTest({
        oplogEntries: [1, 2, 3, 4, 5],
        collectionContents: [1, 2, 3],
        deletePoint: null,
        begin: 3,
        minValid: 6,
        expectedState: 'RECOVERING',
        expectedApplied: [1, 2, 3, 4, 5],
    });

    runTest({
        oplogEntries: [1, 2, 3, 4, 5],
        collectionContents: [1, 2, 3, 4, 5],
        deletePoint: null,
        begin: null,
        minValid: 6,
        expectedState: 'RECOVERING',
        expectedApplied: [1, 2, 3, 4, 5],
    });

    //
    // 3.4 -> 3.2 -> 3.4 downgrade/reupgrade cases
    //

    runTest({
        oplogEntries: [1, 2, 3],
        collectionContents: [1, 2, 3],
        deletePoint: 4,
        begin: 3,
        minValid: 6,
        expectedState: 'RECOVERING',
        expectedApplied: [1, 2, 3],
    });

    runTest({
        oplogEntries: [1, 2, 3, 4, 5],
        collectionContents: [1, 2, 3],
        deletePoint: 4,
        begin: 3,
        minValid: 6,
        expectedState: 'RECOVERING',
        expectedApplied: [1, 2, 3],
    });

    runTest({
        oplogEntries: [1, 2, 3, /*4,*/ 5, 6],
        collectionContents: [1, 2, 3],
        deletePoint: 4,
        begin: 3,
        minValid: 6,
        expectedState: 'RECOVERING',
        expectedApplied: [1, 2, 3],
    });

    runTest({
        oplogEntries: [1, 2, 3],
        collectionContents: [1, 2, 3],
        deletePoint: 2,
        begin: null,
        minValid: 3,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3],
    });

    runTest({
        oplogEntries: [1, 2, 3],
        collectionContents: [1, 2, 3],
        deletePoint: 2,
        begin: 3,
        minValid: 6,
        expectedState: 'RECOVERING',
        expectedApplied: [1, 2, 3],
    });

    runTest({
        oplogEntries: [1, 2, 3, 4, 5],
        collectionContents: [1, 2, 3],
        deletePoint: 2,
        begin: 3,
        minValid: 6,
        expectedState: 'RECOVERING',
        expectedApplied: [1, 2, 3, 4, 5],
    });

    runTest({
        oplogEntries: [1, 2, 3, 4, 5, 6],
        collectionContents: [1, 2, 3],
        deletePoint: 2,
        begin: 3,
        minValid: 6,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3, 4, 5, 6],
    });

    //
    // These states should be impossible to get into.
    //

    runTest({
        oplogEntries: [1, 2, 3],
        collectionContents: [1, 2, 3, 4],
        deletePoint: null,
        begin: 4,
        minValid: null,  // doesn't matter.
        expectedState: 'FATAL',
    });

    runTest({
        oplogEntries: [4, 5, 6],
        collectionContents: [1, 2],
        deletePoint: 2,
        begin: 3,
        minValid: null,  // doesn't matter.
        expectedState: 'FATAL',
    });

    runTest({
        oplogEntries: [4, 5, 6],
        collectionContents: [1, 2],
        deletePoint: null,
        begin: 3,
        minValid: null,  // doesn't matter.
        expectedState: 'FATAL',
    });

    runTest({
        oplogEntries: [1, 2, 3, 4, 5, 6],
        collectionContents: [1, 2, 3],
        deletePoint: 2,
        begin: 3,
        minValid: 3,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3, 4, 5, 6],
    });

    runTest({
        oplogEntries: [1, 2, 3, 4, 5, 6],
        collectionContents: [1, 2, 3, 4, 5],
        deletePoint: null,
        begin: 5,
        minValid: 3,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3, 4, 5, 6],
    });

    runTest({
        oplogEntries: [1, 2, 3, 4, 5, 6],
        collectionContents: [1, 2, 3, 4, 5],
        deletePoint: null,
        begin: 5,
        minValid: null,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3, 4, 5, 6],
    });

    runTest({
        oplogEntries: [1, 2, 3, 4, 5],
        collectionContents: [1],
        deletePoint: 4,
        begin: 1,
        minValid: 3,
        expectedState: 'SECONDARY',
        expectedApplied: [1, 2, 3],
    });

    rst.stopSet();
})();
