/**
 * Tests validating that creating TTL indexes on capped collections is handled correctly on
 * multiversion clusters. On 5.2+, this behavior should not be allowed, but if the FCV version is
 * lower than 5.2, we should allow this.
 *
 * TODO SERVER-61545 This test can be removed when 6.0 is LTS.
 */

(function() {
'use strict';

load("jstests/multiVersion/libs/verify_versions.js");  // For assert.binVersion

function testTTLIndexCreationOnCappedCollection(primaryNodeBinVersion, secondaryNodeBinVersion) {
    // Create mixed version replica set of two nodes.
    const rst = new ReplSetTest({
        nodes: [
            {binVersion: primaryNodeBinVersion},
            {binVersion: secondaryNodeBinVersion, rsConfig: {priority: 0}}
        ]
    });
    rst.startSet();
    rst.initiate();

    // Ensure the bin versions are correct
    assert.binVersion(rst.getPrimary(), primaryNodeBinVersion);
    assert.binVersion(rst.getSecondary(), secondaryNodeBinVersion);

    const dbName = "db";
    const cappedCollName = "collCapped";
    const primary = rst.getPrimary();
    const primaryDb = primary.getDB(dbName);
    const primaryCappedColl = primaryDb.getCollection(cappedCollName);

    // Ensure we can create a TTL index on a capped collection on the primary.
    primaryCappedColl.drop({writeConcern: {w: 2}});
    primaryDb.createCollection(cappedCollName, {capped: true, size: 102400, writeConcern: {w: 2}});

    const res = primaryDb.runCommand({
        createIndexes: cappedCollName,
        indexes: [{key: {a: 1}, name: "a_1", expireAfterSeconds: 10}],
        writeConcern: {w: 2}
    });

    assert.commandWorked(res);

    // Make sure that the index was successfully replicated to the whole replica set.
    rst.nodes.forEach((node) => {
        const db = node.getDB(dbName);
        const res = db.runCommand({"listIndexes": cappedCollName});
        assert(res.cursor.firstBatch[1].hasOwnProperty("expireAfterSeconds"),
               "TTL index was not created properly on capped collection: " +
                   tojson(res.cursor.firstBatch));
    });

    rst.stopSet();
}

testTTLIndexCreationOnCappedCollection("latest" /* primaryNodeBinVersion */,
                                       "5.0" /* secondaryNodeBinVersion */);
testTTLIndexCreationOnCappedCollection("5.0", "latest");
})();
