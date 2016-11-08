/**
 * Test readCommitted lookup/graphLookup. 'db' must be the test database for either the replica set
 * primary or mongos instance. 'secondary' is the shard/replica set secondary. If 'db' is backed
 * by a mongos instance then the associated cluster should have only a single shard. 'rst' is the
 * ReplSetTest instance associated with the replica set/shard.
 */
function testReadCommittedLookup(db, secondary, rst) {
    /**
     * Uses the 'rsSyncApplyStop' fail point to stop application of oplog entries on the given
     * secondary.
     */
    function pauseReplication(sec) {
        assert.commandWorked(
            sec.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "alwaysOn"}),
            "failed to enable fail point on secondary");
    }

    /**
     * Turns off the 'rsSyncApplyStop' fail point to resume application of oplog entries on the
     * given secondary.
     */
    function resumeReplication(sec) {
        assert.commandWorked(sec.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "off"}),
                             "failed to disable fail point on secondary");
    }

    const aggCmdLookupObj = {
        aggregate: "local",
        pipeline: [
            {
              $lookup: {
                  from: "foreign",
                  localField: "foreignKey",
                  foreignField: "matchedField",
                  as: "match",
              }
            },
        ],
        readConcern: {
            level: "majority",
        }
    };

    const aggCmdGraphLookupObj = {
        aggregate: "local",
        pipeline: [{
            $graphLookup: {
                from: "foreign",
                startWith: '$foreignKey',
                connectFromField: 'foreignKey',
                connectToField: "matchedField",
                as: "match"
            }
        }],
        readConcern: {
            level: "majority",
        }
    };

    // Seed matching data.
    const majorityWriteConcernObj = {writeConcern: {w: "majority", wtimeout: 60 * 1000}};
    db.local.deleteMany({}, majorityWriteConcernObj);
    const localId = db.local.insertOne({foreignKey: "x"}, majorityWriteConcernObj).insertedId;
    db.foreign.deleteMany({}, majorityWriteConcernObj);
    const foreignId = db.foreign.insertOne({matchedField: "x"}, majorityWriteConcernObj).insertedId;

    const expectedMatchedResult = [{
        _id: localId,
        foreignKey: "x",
        match: [
            {_id: foreignId, matchedField: "x"},
        ],
    }];
    const expectedUnmatchedResult = [{
        _id: localId,
        foreignKey: "x",
        match: [],
    }];

    // Confirm lookup/graphLookup return the matched result.
    let result = db.runCommand(aggCmdLookupObj).result;
    assert.eq(result, expectedMatchedResult);

    result = db.runCommand(aggCmdGraphLookupObj).result;
    assert.eq(result, expectedMatchedResult);

    // Stop oplog application on the secondary so that it won't acknowledge updates.
    pauseReplication(secondary);

    // Update foreign data to no longer match, without a majority write concern.
    db.foreign.updateOne({_id: foreignId}, {$set: {matchedField: "non-match"}});

    // lookup/graphLookup should not see the update, since it has not been acknowledged by the
    // secondary.
    result = db.runCommand(aggCmdLookupObj).result;
    assert.eq(result, expectedMatchedResult);

    result = db.runCommand(aggCmdGraphLookupObj).result;
    assert.eq(result, expectedMatchedResult);

    // Restart oplog application on the secondary and wait for it's snapshot to catch up.
    resumeReplication(secondary);
    rst.awaitLastOpCommitted();

    // Now lookup/graphLookup should report that the documents don't match.
    result = db.runCommand(aggCmdLookupObj).result;
    assert.eq(result, expectedUnmatchedResult);

    result = db.runCommand(aggCmdGraphLookupObj).result;
    assert.eq(result, expectedUnmatchedResult);
}
