/**
 * Utilies for clustered collections.
 */

function areClusteredIndexesEnabled(conn) {
    const clusteredIndexesEnabled =
        assert.commandWorked(conn.adminCommand({getParameter: 1, featureFlagClusteredIndexes: 1}))
            .featureFlagClusteredIndexes.value;

    if (!clusteredIndexesEnabled) {
        return false;
    }
    return true;
}

function validateClusteredCollection(db, collName, clusterKey) {
    const lengths = [100, 1024, 1024 * 1024, 3 * 1024 * 1024];
    const coll = db[collName];
    const clusterKeyString = new String(clusterKey);

    assert.commandWorked(
        db.createCollection(collName, {clusteredIndex: {key: {[clusterKey]: 1}, unique: true}}));

    // Expect that duplicates are rejected.
    for (let len of lengths) {
        let id = 'x'.repeat(len);
        assert.commandWorked(coll.insert({[clusterKey]: id}));
        assert.commandFailedWithCode(coll.insert({[clusterKey]: id}), ErrorCodes.DuplicateKey);
        assert.eq(1, coll.find({[clusterKey]: id}).itcount());
    }

    // Updates should work.
    for (let len of lengths) {
        let id = 'x'.repeat(len);

        // Validate the below for _id-clustered collection only until the following tickets are
        // addressed:
        // * TODO SERVER-60734 replacement updates should preserve the cluster key
        // * TODO SERVER-60702 enable bounded collscans for arbitary cluster keys
        if (clusterKey == "_id") {
            assert.commandWorked(coll.update({[clusterKey]: id}, {a: len}));

            assert.eq(1, coll.find({[clusterKey]: id}).itcount());
            assert.eq(len, coll.findOne({[clusterKey]: id})['a']);
        }
    }

    // This section is based on jstests/core/timeseries/clustered_index_crud.js with
    // specific additions for general-purpose (non-timeseries) clustered collections
    assert.commandWorked(coll.insert({[clusterKey]: 0, a: 1}));
    assert.commandWorked(coll.insert({[clusterKey]: 1, a: 1}));
    assert.eq(1, coll.find({[clusterKey]: 0}).itcount());
    assert.commandWorked(coll.insert({[clusterKey]: "", a: 2}));
    assert.eq(1, coll.find({[clusterKey]: ""}).itcount());
    assert.commandWorked(coll.insert({[clusterKey]: NumberLong("9223372036854775807"), a: 3}));
    assert.eq(1, coll.find({[clusterKey]: NumberLong("9223372036854775807")}).itcount());
    assert.commandWorked(coll.insert({[clusterKey]: {a: 1, b: 1}, a: 4}));
    assert.eq(1, coll.find({[clusterKey]: {a: 1, b: 1}}).itcount());
    assert.commandWorked(coll.insert({[clusterKey]: {a: {b: 1}, c: 1}, a: 5}));
    assert.commandWorked(coll.insert({[clusterKey]: -1, a: 6}));
    assert.eq(1, coll.find({[clusterKey]: -1}).itcount());
    assert.commandWorked(coll.insert({[clusterKey]: "123456789012", a: 7}));
    assert.eq(1, coll.find({[clusterKey]: "123456789012"}).itcount());
    if (clusterKey == "_id") {
        assert.commandWorked(coll.insert({a: 8}));
    } else {
        // Missing required cluster key field.
        assert.commandFailedWithCode(coll.insert({a: 8}), 2);
        assert.commandWorked(coll.insert({[clusterKey]: "withFieldA", a: 8}));
    }
    assert.eq(1, coll.find({a: 8}).itcount());
    assert.commandWorked(coll.insert({[clusterKey]: null, a: 9}));
    assert.eq(1, coll.find({[clusterKey]: null}).itcount());
    assert.commandWorked(coll.insert({[clusterKey]: 'x'.repeat(99), a: 10}));

    if (clusterKey == "_id") {
        assert.commandWorked(coll.insert({}));
    } else {
        // Missing required ts field.
        assert.commandFailedWithCode(coll.insert({}), 2);
        assert.commandWorked(coll.insert({[clusterKey]: 'missingFieldA'}));
    }
    // Can build a secondary index with a 3MB RecordId doc.
    assert.commandWorked(coll.createIndex({a: 1}));
    // Can drop the secondary index
    assert.commandWorked(coll.dropIndex({a: 1}));

    // This key is too large.
    assert.commandFailedWithCode(coll.insert({[clusterKey]: 'x'.repeat(8 * 1024 * 1024), a: 11}),
                                 5894900);

    // Look up using the secondary index on {a: 1}
    assert.commandWorked(coll.createIndex({a: 1}));

    // TODO remove the branch once SERVER-60734 "replacement updates should preserve the cluster
    // key" is resolved.
    if (clusterKey == "_id") {
        assert.eq(1, coll.find({a: null}).itcount());
    } else {
        assert.eq(5, coll.find({a: null}).itcount());
    }
    assert.eq(0, coll.find({a: 0}).itcount());
    assert.eq(2, coll.find({a: 1}).itcount());
    assert.eq(1, coll.find({a: 2}).itcount());
    assert.eq(1, coll.find({a: 8}).itcount());
    assert.eq(1, coll.find({a: 9}).itcount());
    assert.eq(null, coll.findOne({a: 9})[clusterKeyString]);
    assert.eq(1, coll.find({a: 10}).itcount());
    assert.eq(99, coll.findOne({a: 10})[clusterKeyString].length);

    // todo make it unconditional once server-60734 "replacement updates should preserve the cluster
    // key" is resolved.
    if (clusterKey == "_id") {
        for (let len of lengths) {
            // Secondary index lookups for documents with large RecordId's.
            assert.eq(1, coll.find({a: len}).itcount());
            assert.eq(len, coll.findOne({a: len})[clusterKeyString].length);
        }
    }

    let res = coll.validate();
    assert(res.valid, res);
    assert.commandWorked(coll.dropIndex({a: 1}));

    // Test secondary unique indexes constraints.
    assert.commandFailedWithCode(coll.createIndex({a: 1}, {unique: true}), ErrorCodes.DuplicateKey);
    assert.commandWorked(coll.deleteOne({a: 1}));

    // TODO remove the branch once SERVER-60734 "replacement updates should preserve the cluster
    // key" is resolved.
    if (clusterKey == "_id") {
        assert.eq(1, coll.find({a: null}).itcount());
        assert.commandWorked(coll.removeOne({a: null}));
    } else {
        assert.eq(5, coll.find({a: null}).itcount());
        assert.commandWorked(coll.remove({a: null}, {multi: true}));
    }

    assert.commandWorked(coll.createIndex({a: 1}, {unique: true}));

    // Check that document retrieval using the unique index key behaves correctly for cluster keys
    // of different types.
    assert.eq(0, coll.find({a: 0}).itcount());
    assert.commandWorked(coll.insert({[clusterKey]: 100, a: 0}));

    assert.eq(1, coll.find({a: 1}).itcount());
    assert.commandFailedWithCode(coll.insert({[clusterKey]: -1, a: 1}), ErrorCodes.DuplicateKey);

    assert.eq(1, coll.find({a: 2}).itcount());
    assert.commandFailedWithCode(coll.insert({[clusterKey]: -1, a: 2}), ErrorCodes.DuplicateKey);

    assert.eq(1, coll.find({a: 8}).itcount());
    assert.commandFailedWithCode(coll.insert({[clusterKey]: -1, a: 8}), ErrorCodes.DuplicateKey);

    assert.eq(1, coll.find({a: 9}).itcount());
    assert.eq(null, coll.findOne({a: 9})['_id']);
    assert.commandFailedWithCode(coll.insert({[clusterKey]: -1, a: 9}), ErrorCodes.DuplicateKey);

    assert.eq(1, coll.find({a: 10}).itcount());
    assert.eq(99, coll.findOne({a: 10})['_id'].length);
    assert.commandFailedWithCode(coll.insert({[clusterKey]: -1, a: 10}), ErrorCodes.DuplicateKey);

    // Cycle values of 'a' around to different documents. This intended to ensure correctness of
    // batch application when this test is run in replication passthrough suites.
    assert.commandWorked(coll.insert({[clusterKey]: 10, a: 110}));
    assert.commandWorked(coll.insert({[clusterKey]: 11, a: 111}));
    assert.commandWorked(coll.insert({[clusterKey]: 12, a: 112}));

    assert.commandFailedWithCode(coll.update({[clusterKey]: 12}, {a: 110}),
                                 ErrorCodes.DuplicateKey);
    assert.commandWorked(coll.update({[clusterKey]: 12}, {a: 113}));
    assert.commandWorked(coll.update({[clusterKey]: 11}, {a: 112}));
    assert.commandWorked(coll.update({[clusterKey]: 10}, {a: 111}));
    assert.commandWorked(coll.update({[clusterKey]: 12}, {a: 110}));

    // Unique index lookups for documents with large RecordIds.
    for (let len of lengths) {
        assert.eq(1, coll.find({a: len}).itcount());
        assert.eq(len, coll.findOne({a: len})['_id'].length);
        assert.commandFailedWithCode(coll.insert({a: len}), ErrorCodes.DuplicateKey);
    }

    // No support for numeric type differentiation.
    assert.commandWorked(coll.insert({[clusterKey]: 42.0}));
    assert.commandFailedWithCode(coll.insert({[clusterKey]: 42}), ErrorCodes.DuplicateKey);
    assert.commandFailedWithCode(coll.insert({[clusterKey]: NumberLong("42")}),
                                 ErrorCodes.DuplicateKey);
    assert.eq(1, coll.find({[clusterKey]: 42.0}).itcount());
    assert.eq(1, coll.find({[clusterKey]: 42}).itcount());
    assert.eq(1, coll.find({[clusterKey]: NumberLong("42")}).itcount());
}
