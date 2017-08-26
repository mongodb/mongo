// SERVER-23326: Make applyOps atomic for CRUD operations
(function() {
    'use strict';

    var t = db.applyOps;
    t.drop();
    assert.writeOK(t.insert({_id: 1}));

    // Operations including commands should not be atomic, so the insert will succeed.
    assert.commandFailed(db.adminCommand({
        applyOps: [
            {op: 'i', ns: t.getFullName(), o: {_id: ObjectId(), x: 1}},
            {op: 'c', ns: "invalid", o: {create: "t"}},
        ]
    }));
    assert.eq(t.count({x: 1}), 1);

    // Operations only including CRUD commands should be atomic, so the next insert will fail.
    var tooLong = Array(2000).join("hello");
    assert.commandFailed(db.adminCommand({
        applyOps: [
            {op: 'i', ns: t.getFullName(), o: {_id: ObjectId(), x: 1}},
            {op: 'i', ns: t.getFullName(), o: {_id: tooLong, x: 1}},
        ]
    }));
    assert.eq(t.count({x: 1}), 1);

    // Operations on non-existent databases cannot be atomic.
    var newDBName = "apply_ops_atomicity";
    var newDB = db.getSiblingDB(newDBName);
    assert.commandWorked(newDB.dropDatabase());
    // Updates on a non-existent database no longer implicitly create collections and will fail with
    // a NamespaceNotFound error.
    assert.commandFailedWithCode(newDB.runCommand({
        applyOps: [{op: "u", ns: newDBName + ".foo", o: {_id: 5, x: 17}, o2: {_id: 5, x: 16}}]
    }),
                                 ErrorCodes.NamespaceNotFound);

    var sawTooManyLocksError = false;

    function applyWithManyLocks(n) {
        let cappedOps = [];
        let multiOps = [];

        for (let i = 0; i < n; i++) {
            // Write to a capped collection, as that may require a lock for serialization.
            let cappedName = "capped" + n + "-" + i;
            assert.commandWorked(newDB.createCollection(cappedName, {capped: true, size: 100}));
            cappedOps.push({op: 'i', ns: newDBName + "." + cappedName, o: {_id: 0}});

            // Make an index multi-key, as that may require a lock for updating the catalog.
            let multiName = "multi" + n + "-" + i;
            assert.commandWorked(newDB[multiName].createIndex({x: 1}));
            multiOps.push({op: 'i', ns: newDBName + "." + multiName, o: {_id: 0, x: [0, 1]}});
        }

        let res = [cappedOps, multiOps].map((applyOps) => newDB.runCommand({applyOps}));
        sawTooManyLocksError |= res.some((res) => res.code === ErrorCodes.TooManyLocks);
        // Transactions involving just two collections should succeed.
        if (n <= 2)
            res.every((res) => res.ok);
        // All transactions should either completely succeed or completely fail.
        assert(res.every((res) => res.results.every((result) => result == res.ok)));
        assert(res.every((res) => !res.ok || res.applied == n));
    }

    //  Try requiring different numbers of collection accesses in a single operation to cover
    //  all edge cases, so we run out of available locks in different code paths such as during
    //  oplog application.
    applyWithManyLocks(1);
    applyWithManyLocks(2);

    for (let i = 9; i < 16; i++) {
        applyWithManyLocks(i);
    }
    assert(sawTooManyLocksError, "test no longer exhausts the max number of locks held at once");
})();
