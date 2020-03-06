/**
 * SERVER-46365 In MongoDB v4.0, getMore commands check the featureCompatibilityVersion, but the
 * featureCompatibiltyVersion does not get initialized in a replica set member until the replica set
 * is initialized. The getMore command must not crash when it sees an uninitialized
 * featureCompatibilityVersion, because it is possible to send a getMore to an uninitialized replica
 * set member: for example, by reading the "local" database.
 */
(function() {
    "use strict";

    const rs = new ReplSetTest({name: "getMoreBeforeInitiateTest", nodes: 1});
    rs.startSet();

    // Before replica set initialization finishes, members reject getMore commands for any database
    // other than the "local" database.
    const db = rs.nodes[0].getDB("local");
    const collName = "non_existent_collection";

    // We use batch size 0 to ensure we get a cursor, even though there is no data in the
    // collection.
    const aggregateRes = assert.commandWorked(
        db.runCommand({aggregate: collName, pipeline: [], cursor: {batchSize: 0}}));
    assert(aggregateRes.hasOwnProperty("cursor"), aggregateRes);

    // This getMore command will check the featureCompatibiltyVersion.
    const getMoreRes = assert.commandWorked(
        db.runCommand({getMore: aggregateRes.cursor.id, collection: collName}));

    // This test is primarily testing that we don't crash, but we may as well also include a sanity
    // check that we get back a correct (empty) result and exhausted cursor.
    assert(getMoreRes.hasOwnProperty("cursor"), getMoreRes);
    assert.eq(getMoreRes.cursor.id, 0, getMoreRes);
    assert.eq(getMoreRes.cursor.nextBatch, [], getMoreRes);

    rs.stopSet();
}());