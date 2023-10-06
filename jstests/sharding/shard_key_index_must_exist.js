(function() {
'use strict';

let st = new ShardingTest({shards: 1});

assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));

let testDB = st.getDB('test');

let checkIndex = function(collName, expectedIndexNames) {
    const indexes = testDB.getCollection(collName).getIndexes();
    indexes.forEach(index => {
        assert(expectedIndexNames.includes(index.name), 'index should not exist: ' + tojson(index));

        expectedIndexNames = expectedIndexNames.filter(name => name !== index.name);
    });

    assert.eq([], expectedIndexNames);
};

(() => {
    assert.commandWorked(st.s.adminCommand({shardCollection: 'test.user', key: {x: 1}}));
    assert.commandFailedWithCode(testDB.runCommand({dropIndexes: 'user', index: {x: 1}}),
                                 ErrorCodes.CannotDropShardKeyIndex);

    assert.commandWorked(testDB.runCommand({
        createIndexes: 'user',
        indexes: [
            {key: {x: 1, y: 1}, name: 'xy'},
            {key: {x: 1, z: 1}, name: 'xz'},
            {key: {a: 1}, name: 'a'}
        ]
    }));

    assert.commandWorked(testDB.runCommand({dropIndexes: 'user', index: '*'}));
    checkIndex('user', ['_id_', 'x_1', 'xy', 'xz']);

    assert.commandWorked(testDB.runCommand({dropIndexes: 'user', index: {x: 1}}));
    assert.commandWorked(testDB.runCommand({dropIndexes: 'user', index: {x: 1, z: 1}}));

    assert.commandFailedWithCode(testDB.runCommand({dropIndexes: 'user', index: {x: 1, y: 1}}),
                                 ErrorCodes.CannotDropShardKeyIndex);
    assert.commandFailedWithCode(testDB.runCommand({dropIndexes: 'user', index: 'xy'}),
                                 ErrorCodes.CannotDropShardKeyIndex);

    checkIndex('user', ['_id_', 'xy']);

    // Check dropping indexes in a non-empty collection.
    assert.commandWorked(testDB.runCommand({insert: 'user', documents: [{x: 1}]}));
    assert.commandFailedWithCode(testDB.runCommand({dropIndexes: 'user', index: {x: 1, y: 1}}),
                                 ErrorCodes.CannotDropShardKeyIndex);
    assert.commandFailedWithCode(testDB.runCommand({dropIndexes: 'user', index: 'xy'}),
                                 ErrorCodes.CannotDropShardKeyIndex);

    checkIndex('user', ['_id_', 'xy']);
    assert.commandWorked(testDB.runCommand({drop: 'user'}));
})();

(() => {
    const featureFlagDropHashedShardKeyIndexes =
        st.shard0.getDB('admin')
            .adminCommand({getParameter: 1, featureFlagShardKeyIndexOptionalHashedSharding: 1})
            .featureFlagShardKeyIndexOptionalHashedSharding.value;
    if (featureFlagDropHashedShardKeyIndexes) {
        // Users are allowed to drop hashed shard key indexes. This includes any compound index
        // that is prefixed by the hashed shard key.
        assert.commandWorked(
            st.s.adminCommand({shardCollection: 'test.hashed', key: {x: 'hashed'}}));

        assert.commandWorked(testDB.runCommand(
            {createIndexes: 'hashed', indexes: [{key: {x: 1, y: 1}, name: 'xy'}]}));
        // This will also drop the hashed shard key index.
        assert.commandWorked(testDB.runCommand({dropIndexes: 'hashed', index: '*'}));
        checkIndex('hashed', ['_id_']);

        assert.commandWorked(testDB.runCommand(
            {createIndexes: 'hashed', indexes: [{key: {x: 'hashed'}, name: 'x_hashed'}]}));
        assert.commandWorked(testDB.runCommand({dropIndexes: 'hashed', index: 'x_hashed'}));
        checkIndex('hashed', ['_id_']);

        assert.commandWorked(testDB.runCommand({
            createIndexes: 'hashed',
            indexes: [{key: {x: 'hashed', y: 1}, name: 'x_hashed_y_1'}]
        }));
        assert.commandWorked(testDB.runCommand({dropIndexes: 'hashed', index: '*'}));
        checkIndex('hashed', ['_id_']);

        assert.commandWorked(testDB.runCommand({
            createIndexes: 'hashed',
            indexes: [{key: {x: 'hashed', y: 1}, name: 'x_hashed_y_1'}]
        }));
        assert.commandWorked(testDB.runCommand({dropIndexes: 'hashed', index: 'x_hashed_y_1'}));
        checkIndex('hashed', ['_id_']);

        // Check dropping indexes in a non-empty collection.
        assert.commandWorked(testDB.runCommand(
            {createIndexes: 'hashed', indexes: [{key: {x: 'hashed'}, name: 'x_hashed'}]}));
        assert.commandWorked(testDB.runCommand({insert: 'hashed', documents: [{x: 1}]}));
        assert.commandWorked(testDB.runCommand({dropIndexes: 'hashed', index: 'x_hashed'}));
        checkIndex('hashed', ['_id_']);

        assert.commandWorked(testDB.runCommand({
            createIndexes: 'hashed',
            indexes: [{key: {x: 'hashed', y: 1}, name: 'x_hashed_y_1'}]
        }));
        assert.commandWorked(testDB.runCommand({dropIndexes: 'hashed', index: '*'}));
        checkIndex('hashed', ['_id_']);
    } else {
        assert.commandWorked(
            st.s.adminCommand({shardCollection: 'test.hashed', key: {x: 'hashed'}}));

        assert.commandWorked(testDB.runCommand(
            {createIndexes: 'hashed', indexes: [{key: {x: 1, y: 1}, name: 'xy'}]}));

        assert.commandWorked(testDB.runCommand({dropIndexes: 'hashed', index: '*'}));

        checkIndex('hashed', ['_id_', 'x_hashed']);

        assert.commandFailedWithCode(testDB.runCommand({dropIndexes: 'hashed', index: 'x_hashed'}),
                                     ErrorCodes.CannotDropShardKeyIndex);

        checkIndex('hashed', ['_id_', 'x_hashed']);
    }

    assert.commandWorked(testDB.runCommand({drop: 'hashed'}));
})();

st.stop();
})();
