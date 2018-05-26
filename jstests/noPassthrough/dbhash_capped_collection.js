/**
 * Tests that the dbHash command separately lists the names of capped collections on the database.
 *
 * @tags: [requires_replication]
 */
(function() {
    "use strict";

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const db = rst.getPrimary().getDB("test");

    // We create a capped collection as well as a non-capped collection and verify that the "capped"
    // field in the dbHash command response only lists the capped one.
    assert.commandWorked(db.runCommand({create: "noncapped"}));
    assert.commandWorked(db.runCommand({create: "capped", capped: true, size: 4096}));
    let res = assert.commandWorked(db.runCommand({dbHash: 1}));
    assert.eq(["capped"], res.capped);

    // If the capped collection is excluded from the list of collections to md5sum, then it won't
    // appear in the "capped" field either.
    res = assert.commandWorked(db.runCommand({dbHash: 1, collections: ["noncapped"]}));
    assert.eq([], res.capped);

    {
        const session = db.getMongo().startSession();

        const hashesDefault = rst.getHashesUsingSessions([session], db.getName());
        const hashesFilterCapped =
            rst.getHashesUsingSessions([session], db.getName(), {filterCapped: true});
        const hashesNoFilterCapped =
            rst.getHashesUsingSessions([session], db.getName(), {filterCapped: false});

        assert.eq(["noncapped"],
                  Object.keys(hashesFilterCapped[0].collections),
                  "capped collection should have been filtered out");
        assert.eq(["capped", "noncapped"],
                  Object.keys(hashesNoFilterCapped[0].collections).sort(),
                  "capped collection should not have been filtered out");
        assert.eq(hashesDefault[0].collections,
                  hashesFilterCapped[0].collections,
                  "getHashesUsingSessions() should default to filter out capped collections");

        const hashes = rst.getHashes(db.getName());
        assert.eq(hashesNoFilterCapped[0].collections,
                  hashes.master.collections,
                  "getHashes() should default to not filter out capped collections");

        session.endSession();
    }

    rst.stopSet();
})();
