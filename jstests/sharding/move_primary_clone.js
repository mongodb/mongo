(function() {
'use strict';

function sortByName(a, b) {
    if (a.name < b.name)
        return -1;
    if (a.name > b.name)
        return 1;
    return 0;
}

function getCollections(shard) {
    var res = shard.getDB("test1").runCommand({listCollections: 1});
    assert.commandWorked(res);

    var collections = res.cursor.firstBatch;

    // Sort collections by name.
    collections.sort(sortByName);
    assert.eq(collections.length, 2);

    return collections;
}

function checkOptions(c, expectedOptions) {
    assert.hasFields(c, ['options'], 'Missing options field for collection ' + c.name);
    assert.hasFields(
        c.options, expectedOptions, 'Missing expected option(s) for collection ' + c.name);
}

function checkCollectionsCopiedCorrectly(fromShard, toShard, sharded, barUUID, fooUUID) {
    var c1, c2;
    [c1, c2] = getCollections(toShard);

    function checkName(c, expectedName) {
        assert.eq(
            c.name, expectedName, 'Expected collection to be ' + expectedName + ', got ' + c.name);
    }

    function checkUUIDsEqual(c, expectedUUID) {
        assert.hasFields(c, ['info'], 'Missing info field for collection ' + c.name);
        assert.hasFields(c.info, ['uuid'], 'Missing uuid field for collection ' + c.name);
        assert.eq(c.info.uuid, expectedUUID, 'Incorrect uuid for collection ' + c.name);
    }

    function checkUUIDsNotEqual(c, originalUUID) {
        assert.hasFields(c, ['info'], 'Missing info field for collection ' + c.name);
        assert.hasFields(c.info, ['uuid'], 'Missing uuid field for collection ' + c.name);
        assert.neq(c.info.uuid,
                   originalUUID,
                   'UUID for ' + c.name +
                       ' should be different than the original collection but is the same');
    }

    function checkIndexes(collName, expectedIndexes) {
        var res = toShard.getDB('test1').runCommand({listIndexes: collName});
        assert.commandWorked(res, 'Failed to get indexes for collection ' + collName);
        var indexes = res.cursor.firstBatch;
        indexes.sort(sortByName);

        // TODO SERVER-74252: once 7.0 becomes LastLTS we can assume that the movePrimary will never
        // copy indexes of sharded collections.
        if (sharded)
            assert(indexes.length == 1 || indexes.length == 2);
        else
            assert(indexes.length == 2);

        indexes.forEach((index, i) => {
            var expected;
            if (i == 0) {
                expected = {name: "_id_", key: {_id: 1}};
            } else {
                expected = expectedIndexes[i - 1];
            }
            Object.keys(expected).forEach(k => {
                assert.eq(index[k], expected[k]);
            });
        });
    }

    function checkCount(shard, collName, count) {
        var res = shard.getDB('test1').runCommand({count: collName});
        assert.commandWorked(res);
        assert.eq(res.n, count);
    }

    checkName(c1, 'bar');
    checkName(c2, 'foo');
    checkOptions(c1, Object.keys(barOptions));
    checkIndexes('bar', barIndexes);
    checkOptions(c2, Object.keys(fooOptions));
    checkIndexes('foo', fooIndexes);

    if (sharded) {
        checkCount(fromShard, 'foo', 3);
        checkCount(fromShard, 'bar', 3);
        checkCount(toShard, 'foo', 0);
        checkCount(toShard, 'bar', 0);

        // UUIDs should be the same as the original
        checkUUIDsEqual(c1, barUUID);
        checkUUIDsEqual(c2, fooUUID);
    } else {
        checkCount(toShard, 'foo', 3);
        checkCount(toShard, 'bar', 3);
        checkCount(fromShard, 'foo', 0);
        checkCount(fromShard, 'bar', 0);

        // UUIDs should not be the same as the original
        checkUUIDsNotEqual(c1, barUUID);
        checkUUIDsNotEqual(c2, fooUUID);
    }
}

function createCollections(sharded) {
    assert.commandWorked(st.getDB('test1').runCommand({dropDatabase: 1}));
    var db = st.getDB('test1');

    assert.commandWorked(db.createCollection('foo', fooOptions));
    assert.commandWorked(db.createCollection('bar', barOptions));

    for (let i = 0; i < 3; i++) {
        assert.commandWorked(db.foo.insert({a: i}));
        assert.commandWorked(db.bar.insert({a: i}));
    }
    assert.eq(3, db.foo.count());
    assert.eq(3, db.bar.count());

    assert.commandWorked(db.runCommand({createIndexes: 'foo', indexes: fooIndexes}));
    assert.commandWorked(db.runCommand({createIndexes: 'bar', indexes: barIndexes}));

    if (sharded) {
        assert.commandWorked(db.adminCommand({enableSharding: 'test1'}));
        assert.commandWorked(db.adminCommand({shardCollection: 'test1.foo', key: {_id: 1}}));
        assert.commandWorked(db.adminCommand({shardCollection: 'test1.bar', key: {_id: 1}}));
    }
}

function movePrimaryWithFailpoint(sharded) {
    var db = st.getDB('test1');
    createCollections(sharded);

    var fromShard = st.getPrimaryShard('test1');
    var toShard = st.getOther(fromShard);

    assert.eq(3, fromShard.getDB("test1").foo.count(), "from shard doesn't have data before move");
    assert.eq(0, toShard.getDB("test1").foo.count(), "to shard has data before move");
    assert.eq(3, fromShard.getDB("test1").bar.count(), "from shard doesn't have data before move");
    assert.eq(0, toShard.getDB("test1").bar.count(), "to shard has data before move");

    var listCollsFrom = fromShard.getDB("test1").runCommand({listCollections: 1});
    var fromColls = listCollsFrom.cursor.firstBatch;
    fromColls.sort(sortByName);
    var baruuid = fromColls[0].info.uuid;
    var foouuid = fromColls[1].info.uuid;

    assert.commandWorked(toShard.getDB("admin").runCommand(
        {configureFailPoint: 'movePrimaryFailPoint', mode: 'alwaysOn'}));

    // Failpoint will cause movePrimary to fail after the first collection has been copied over
    assert.commandFailed(st.s0.adminCommand({movePrimary: "test1", to: toShard.name}));

    assert.commandWorked(toShard.getDB("admin").runCommand(
        {configureFailPoint: 'movePrimaryFailPoint', mode: 'off'}));

    if (sharded) {
        // If the collections are sharded, the UUID of the collection on the donor should be
        // copied over and the options should be the same so retrying the move should succeed.
        assert.commandWorked(st.s0.adminCommand({movePrimary: "test1", to: toShard.name}));
        checkCollectionsCopiedCorrectly(fromShard, toShard, sharded, baruuid, foouuid);

        // Now change an option on the toShard, and verify that calling clone again succeeds
        // when the options don't match.
        assert.commandWorked(
            toShard.getDB('test1').runCommand({collMod: 'bar', validationLevel: 'moderate'}));
        assert.commandWorked(st.s0.adminCommand({movePrimary: "test1", to: fromShard.name}));

        // Assert that the fromShard does not have the new options, but the toShard does
        let barOnToShard = getCollections(toShard)[0];
        checkOptions(
            barOnToShard,
            Object.keys(
                {validator: {$jsonSchema: {required: ['a']}}, validationLevel: "moderate"}));
        let barOnFromShard = getCollections(fromShard)[0];
        checkOptions(barOnFromShard, Object.keys(barOptions));

        // The docs should still be on the original primary shard (fromShard).
        checkCollectionsCopiedCorrectly(fromShard, toShard, sharded, baruuid, foouuid);

        // Now drop and recreate the collection on the toShard. The collection should now have
        // a different UUID, so we should fail on movePrimary.
        assert.commandWorked(toShard.getDB("test1").runCommand({drop: "bar"}));
        assert.commandWorked(toShard.getDB("test1").bar.insert({"x": 1}));

        assert.commandFailedWithCode(st.s0.adminCommand({movePrimary: "test1", to: toShard.name}),
                                     ErrorCodes.InvalidOptions);
    } else {
        // The failure of the previous attempt caused the dirty data on the recipient to be dropped,
        // so the data cloning shouldn't find any impediments.
        assert.commandWorked(st.s0.adminCommand({movePrimary: "test1", to: toShard.name}));
    }
}

function movePrimaryNoFailpoint(sharded) {
    var db = st.getDB('test1');
    createCollections(sharded);

    var fromShard = st.getPrimaryShard('test1');
    var toShard = st.getOther(fromShard);

    assert.eq(3, fromShard.getDB("test1").foo.count(), "from shard doesn't have data before move");
    assert.eq(0, toShard.getDB("test1").foo.count(), "to shard has data before move");
    assert.eq(3, fromShard.getDB("test1").bar.count(), "from shard doesn't have data before move");
    assert.eq(0, toShard.getDB("test1").bar.count(), "to shard has data before move");

    var listCollsFrom = fromShard.getDB("test1").runCommand({listCollections: 1});
    var fromColls = listCollsFrom.cursor.firstBatch;
    fromColls.sort(sortByName);
    var baruuid = fromColls[0].info.uuid;
    var foouuid = fromColls[1].info.uuid;

    assert.commandWorked(st.s0.adminCommand({movePrimary: "test1", to: toShard.name}));

    checkCollectionsCopiedCorrectly(fromShard, toShard, sharded, baruuid, foouuid);
}

var st = new ShardingTest({shards: 2});

var fooOptions = {validationLevel: "off"};
var barOptions = {validator: {$jsonSchema: {required: ['a']}}};

var fooIndexes = [{key: {a: 1}, name: 'index1', expireAfterSeconds: 5000}];
var barIndexes = [{key: {a: -1}, name: 'index2'}];

movePrimaryWithFailpoint(true);
movePrimaryWithFailpoint(false);
movePrimaryNoFailpoint(true);
movePrimaryNoFailpoint(false);

st.stop();
})();
