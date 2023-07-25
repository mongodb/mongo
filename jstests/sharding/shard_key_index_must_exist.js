(function() {
'use strict';

let st = new ShardingTest({shards: 1});

assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));

let testDB = st.getDB('test');

let checkIndex = function(collName, expectedIndexNames) {
    let indexNotSeen = expectedIndexNames;

    testDB.getCollection(collName).getIndexes().forEach((index) => {
        assert(expectedIndexNames.includes(index.name),
               'index should not expected to exist: ' + tojson(index));

        indexNotSeen = indexNotSeen.filter((name) => {
            return name == index.name;
        });
    });

    assert.eq([], indexNotSeen);
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

    assert.commandWorked(testDB.runCommand({drop: 'user'}));
})();

(() => {
    assert.commandWorked(st.s.adminCommand({shardCollection: 'test.hashed', key: {x: 'hashed'}}));

    assert.commandWorked(
        testDB.runCommand({createIndexes: 'hashed', indexes: [{key: {x: 1, y: 1}, name: 'xy'}]}));

    assert.commandWorked(testDB.runCommand({dropIndexes: 'hashed', index: '*'}));

    checkIndex('hashed', ['_id_', 'x_hashed']);

    assert.commandFailedWithCode(testDB.runCommand({dropIndexes: 'hashed', index: 'x_hashed'}),
                                 ErrorCodes.CannotDropShardKeyIndex);

    checkIndex('hashed', ['_id_', 'x_hashed']);

    assert.commandWorked(testDB.runCommand({drop: 'hashed'}));
})();

st.stop();
})();
