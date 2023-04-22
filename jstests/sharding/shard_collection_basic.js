(function() {
'use strict';

load("jstests/sharding/libs/find_chunks_util.js");

var st = new ShardingTest({shards: 2});
var kDbName = 'db';
var mongos = st.s0;
var config = st.s0.getDB('config');
var admin = st.s0.getDB('admin');

function testAndClenaupWithKeyNoIndexFailed(keyDoc) {
    assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));

    var ns = kDbName + '.foo';
    assert.commandFailed(mongos.adminCommand({shardCollection: ns, key: keyDoc}));

    assert.eq(mongos.getDB('config').collections.count({_id: ns}), 0);
    assert.commandWorked(mongos.getDB(kDbName).dropDatabase());
}

function testAndClenaupWithKeyOK(keyDoc) {
    assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
    assert.commandWorked(mongos.getDB(kDbName).foo.createIndex(keyDoc));

    var ns = kDbName + '.foo';
    assert.eq(mongos.getDB('config').collections.count({_id: ns}), 0);

    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: keyDoc}));

    assert.eq(mongos.getDB('config').collections.count({_id: ns}), 1);
    assert.commandWorked(mongos.getDB(kDbName).dropDatabase());
}

function testAndClenaupWithKeyNoIndexOK(keyDoc) {
    assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));

    var ns = kDbName + '.foo';
    assert.eq(mongos.getDB('config').collections.count({_id: ns}), 0);

    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: keyDoc}));

    assert.eq(mongos.getDB('config').collections.count({_id: ns}), 1);
    assert.commandWorked(mongos.getDB(kDbName).dropDatabase());
}

function getIndexSpecByName(coll, indexName) {
    var indexes = coll.getIndexes().filter(function(spec) {
        return spec.name === indexName;
    });
    assert.eq(1, indexes.length, 'index "' + indexName + '" not found"');
    return indexes[0];
}

assert.commandWorked(mongos.getDB(kDbName).foo.insert({a: 1, b: 1}));
assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));

jsTestLog('Verify wrong arguments errors.');
assert.commandFailed(mongos.adminCommand({shardCollection: 'foo', key: {_id: 1}}));

assert.commandFailed(mongos.adminCommand({shardCollection: kDbName + '.foo', key: "aaa"}));

const testDB = st.rs0.getPrimary().getDB(kDbName);
const fcvDoc = testDB.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
if (MongoRunner.compareBinVersions(fcvDoc.featureCompatibilityVersion.version, '5.3') >= 0) {
    jsTestLog('Verify namespace length limit.');

    const longEnoughNs = kDbName + '.' +
        'x'.repeat(235 - kDbName.length - 1);
    assert.commandWorked(mongos.adminCommand({shardCollection: longEnoughNs, key: {_id: 1}}));

    const tooLongNs = longEnoughNs + 'x';
    assert.commandFailedWithCode(mongos.adminCommand({shardCollection: tooLongNs, key: {_id: 1}}),
                                 ErrorCodes.InvalidNamespace);
}

{
    jsTestLog("Special collections can't be sharded");

    let specialColls = [
        'config.foo',             // all collections in config db except config.system.sessions
        `${kDbName}.system.foo`,  // any custom system collection in any db
    ];

    // TODO BACKPORT-15485 always enable check on admin collections once the backport is completed
    if (!jsTest.options().shardMixedBinVersions &&
        !jsTest.options().useRandomBinVersionsWithinReplicaSet) {
        specialColls.push('admin.foo');  // all collections in admin db can't be sharded
    }

    specialColls.forEach(collName => {
        assert.commandFailedWithCode(
            mongos.adminCommand({shardCollection: collName, key: {_id: 1}}),
            ErrorCodes.IllegalOperation);
    });

    // For collections in the local database the router will attempt to create the db and it will
    // fail with InvalidOptions
    assert.commandFailedWithCode(mongos.adminCommand({shardCollection: 'local.foo', key: {_id: 1}}),
                                 ErrorCodes.InvalidOptions);
}

jsTestLog('shardCollection may only be run against admin database.');
assert.commandFailed(
    mongos.getDB('test').runCommand({shardCollection: kDbName + '.foo', key: {_id: 1}}));

assert.commandWorked(mongos.getDB(kDbName).foo.insert({a: 1, b: 1}));

jsTestLog('Cannot shard if key is not specified.');
assert.commandFailed(mongos.adminCommand({shardCollection: kDbName + '.foo'}));

assert.commandFailed(mongos.adminCommand({shardCollection: kDbName + '.foo', key: {}}));

jsTestLog('Verify key format.');
assert.commandFailed(
    mongos.adminCommand({shardCollection: kDbName + '.foo', key: {aKey: 'hahahashed'}}));

jsTestLog('Shard key cannot contain embedded objects.');
assert.commandFailed(mongos.adminCommand({shardCollection: kDbName + '.foo', key: {_id: {a: 1}}}));
assert.commandFailed(
    mongos.adminCommand({shardCollection: kDbName + '.foo', key: {_id: {'a.b': 1}}}));

jsTestLog('Shard key can contain dotted path to embedded element.');
assert.commandWorked(
    mongos.adminCommand({shardCollection: kDbName + '.shard_key_dotted_path', key: {'_id.a': 1}}));

jsTestLog('Command should still verify index even if implicitlyCreateIndex is false.');
assert.commandFailedWithCode(
    mongos.adminCommand(
        {shardCollection: kDbName + '.foo', key: {x: 1}, implicitlyCreateIndex: false}),
    6373200);

//
// Test shardCollection's idempotency
//

jsTestLog('Succeed if a collection is already sharded with the same options.');
assert.commandWorked(mongos.adminCommand({shardCollection: kDbName + '.foo', key: {_id: 1}}));
assert.commandWorked(mongos.adminCommand({shardCollection: kDbName + '.foo', key: {_id: 1}}));

jsTestLog(
    'Specifying the simple collation or not specifying a collation should be equivalent, because if no collation is specified, the collection default collation is used.');
assert.commandWorked(mongos.adminCommand(
    {shardCollection: kDbName + '.foo', key: {_id: 1}, collation: {locale: 'simple'}}));

jsTestLog('Fail if the collection is already sharded with different options. different shard key');
assert.commandFailed(mongos.adminCommand({shardCollection: kDbName + '.foo', key: {x: 1}}));

jsTestLog('Different "unique"');
assert.commandFailed(
    mongos.adminCommand({shardCollection: kDbName + '.foo', key: {_id: 1}, unique: true}));

assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

jsTestLog('Allow non-unique index if enforceUniquenessCheck is false');
assert.commandWorked(mongos.getDB(kDbName).foo.createIndex({x: 1}));
assert.commandWorked(mongos.adminCommand(
    {shardCollection: kDbName + '.foo', key: {x: 1}, unique: true, enforceUniquenessCheck: false}));
let collDoc = mongos.getDB('config').collections.findOne({_id: `${kDbName}.foo`});
assert(collDoc);
assert(collDoc.unique);
assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

jsTestLog('mongosync unique key pattern use case');
assert.commandWorked(mongos.getDB(kDbName).foo.createIndex({x: 1}));
assert.commandWorked(mongos.adminCommand({
    shardCollection: kDbName + '.foo',
    key: {x: 1},
    unique: true,
    implicitlyCreateIndex: false,
    enforceUniquenessCheck: false
}));
collDoc = mongos.getDB('config').collections.findOne({_id: `${kDbName}.foo`});
assert(collDoc);
assert(collDoc.unique);
assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

jsTestLog('Shard empty collections no index required.');
testAndClenaupWithKeyNoIndexOK({_id: 1});
testAndClenaupWithKeyNoIndexOK({_id: 'hashed'});

jsTestLog('Shard by a plain key.');
testAndClenaupWithKeyNoIndexOK({a: 1});

jsTestLog('Cant shard collection with data and no index on the shard key.');
assert.commandWorked(mongos.getDB(kDbName).foo.insert({a: 1, b: 1}));
testAndClenaupWithKeyNoIndexFailed({a: 1});

assert.commandWorked(mongos.getDB(kDbName).foo.insert({a: 1, b: 1}));
testAndClenaupWithKeyOK({a: 1});

jsTestLog('Shard by a hashed key.');
testAndClenaupWithKeyNoIndexOK({a: 'hashed'});

assert.commandWorked(mongos.getDB(kDbName).foo.insert({a: 1, b: 1}));
testAndClenaupWithKeyNoIndexFailed({a: 'hashed'});

assert.commandWorked(mongos.getDB(kDbName).foo.insert({a: 1, b: 1}));
testAndClenaupWithKeyOK({a: 'hashed'});

jsTestLog('Shard by a compound key.');
testAndClenaupWithKeyNoIndexOK({x: 1, y: 1});

assert.commandWorked(mongos.getDB(kDbName).foo.insert({x: 1, y: 1}));
testAndClenaupWithKeyNoIndexFailed({x: 1, y: 1});

assert.commandWorked(mongos.getDB(kDbName).foo.insert({x: 1, y: 1}));
testAndClenaupWithKeyOK({x: 1, y: 1});

jsTestLog('Multiple hashed fields are not allowed.');
testAndClenaupWithKeyNoIndexFailed({x: 'hashed', a: 1, y: 'hashed'});
testAndClenaupWithKeyNoIndexFailed({x: 'hashed', y: 'hashed'});

jsTestLog('Negative numbers are not allowed.');
testAndClenaupWithKeyNoIndexFailed({x: 'hashed', a: -1});

jsTestLog('Shard by a key component.');
testAndClenaupWithKeyOK({'z.x': 1});
testAndClenaupWithKeyOK({'z.x': 'hashed'});

jsTestLog('Cannot shard by a multikey.');
assert.commandWorked(mongos.getDB(kDbName).foo.createIndex({a: 1}));
assert.commandWorked(mongos.getDB(kDbName).foo.insert({a: [1, 2, 3, 4, 5], b: 1}));
testAndClenaupWithKeyNoIndexFailed({a: 1});

assert.commandWorked(mongos.getDB(kDbName).foo.createIndex({a: 1, b: 1}));
assert.commandWorked(mongos.getDB(kDbName).foo.insert({a: [1, 2, 3, 4, 5], b: 1}));
testAndClenaupWithKeyNoIndexFailed({a: 1, b: 1});

assert.commandWorked(mongos.getDB(kDbName).foo.insert({a: 1, b: 1}));
testAndClenaupWithKeyNoIndexFailed({a: 'hashed'});

assert.commandWorked(mongos.getDB(kDbName).foo.insert({a: 1, b: 1}));
testAndClenaupWithKeyOK({a: 'hashed'});

jsTestLog('Cannot shard by a parallel arrays.');
assert.commandWorked(mongos.getDB(kDbName).foo.insert({a: [1, 2, 3, 4, 5], b: [1, 2, 3, 4, 5]}));
testAndClenaupWithKeyNoIndexFailed({a: 1, b: 1});

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));

jsTestLog('Cannot shard on unique hashed key.');
assert.commandFailed(
    mongos.adminCommand({shardCollection: kDbName + '.foo', key: {aKey: "hashed"}, unique: true}));

jsTestLog('If shardCollection has unique:true it  must have a unique index.');
assert.commandWorked(mongos.getDB(kDbName).foo.createIndex({aKey: 1}));

assert.commandFailed(
    mongos.adminCommand({shardCollection: kDbName + '.foo', key: {aKey: 1}, unique: true}));

//
// Session-related tests
//

assert.commandWorked(mongos.getDB(kDbName).dropDatabase());
assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));

jsTestLog('shardCollection can be called under a session.');
const sessionDb = mongos.startSession().getDatabase(kDbName);
assert.commandWorked(
    sessionDb.adminCommand({shardCollection: kDbName + '.foo', key: {_id: 'hashed'}}));
sessionDb.getSession().endSession();

assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

//
// Collation-related tests
//

assert.commandWorked(mongos.getDB(kDbName).dropDatabase());
assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));

jsTestLog('shardCollection should fail when the "collation" option is not a nested object.');
assert.commandFailed(
    mongos.adminCommand({shardCollection: kDbName + '.foo', key: {_id: 1}, collation: true}));

jsTestLog('shardCollection should fail when the "collation" option cannot be parsed.');
assert.commandFailed(mongos.adminCommand(
    {shardCollection: kDbName + '.foo', key: {_id: 1}, collation: {locale: 'unknown'}}));

jsTestLog(
    'shardCollection should fail when the "collation" option is valid but is not the simple collation.');
assert.commandFailed(mongos.adminCommand(
    {shardCollection: kDbName + '.foo', key: {_id: 1}, collation: {locale: 'en_US'}}));

jsTestLog(
    'shardCollection should succeed when the "collation" option specifies the simple collation.');
assert.commandWorked(mongos.adminCommand(
    {shardCollection: kDbName + '.foo', key: {_id: 1}, collation: {locale: 'simple'}}));

jsTestLog(
    'shardCollection should fail when it does not specify the "collation" option but the collection has a non-simple default collation.');
mongos.getDB(kDbName).foo.drop();
assert.commandWorked(mongos.getDB(kDbName).createCollection('foo', {collation: {locale: 'en_US'}}));
assert.commandFailed(mongos.adminCommand({shardCollection: kDbName + '.foo', key: {a: 1}}));

jsTestLog(
    'shardCollection should fail for the key pattern {_id: 1} if the collection has a non-simple default collation.');
mongos.getDB(kDbName).foo.drop();
assert.commandWorked(mongos.getDB(kDbName).createCollection('foo', {collation: {locale: 'en_US'}}));
assert.commandFailed(mongos.adminCommand(
    {shardCollection: kDbName + '.foo', key: {_id: 1}, collation: {locale: 'simple'}}));

jsTestLog(
    'shardCollection should fail for the key pattern {a: 1} if there is already an index "a_1", but it has a non-simple collation.');
mongos.getDB(kDbName).foo.drop();
assert.commandWorked(mongos.getDB(kDbName).foo.createIndex({a: 1}, {collation: {locale: 'en_US'}}));
assert.commandFailed(mongos.adminCommand({shardCollection: kDbName + '.foo', key: {a: 1}}));

jsTestLog(
    'shardCollection should succeed for the key pattern {a: 1} and collation {locale: "simple"} if there is no index "a_1", but there is a non-simple collection default collation.');
mongos.getDB(kDbName).foo.drop();
assert.commandWorked(mongos.getDB(kDbName).createCollection('foo', {collation: {locale: 'en_US'}}));
assert.commandWorked(mongos.adminCommand(
    {shardCollection: kDbName + '.foo', key: {a: 1}, collation: {locale: 'simple'}}));
var indexSpec = getIndexSpecByName(mongos.getDB(kDbName).foo, 'a_1');
assert(!indexSpec.hasOwnProperty('collation'));

jsTestLog(
    'shardCollection should succeed for the key pattern {a: 1} if there are two indexes on {a: 1} and one has the simple collation.');
mongos.getDB(kDbName).foo.drop();
assert.commandWorked(mongos.getDB(kDbName).foo.createIndex({a: 1}, {name: "a_1_simple"}));
assert.commandWorked(mongos.getDB(kDbName).foo.createIndex(
    {a: 1}, {collation: {locale: 'en_US'}, name: "a_1_en_US"}));
assert.commandWorked(mongos.adminCommand({shardCollection: kDbName + '.foo', key: {a: 1}}));

jsTestLog(
    'shardCollection should fail on a non-empty collection when the only index available with the shard key as a prefix has a non-simple collation.');
mongos.getDB(kDbName).foo.drop();
assert.commandWorked(mongos.getDB(kDbName).createCollection('foo', {collation: {locale: 'en_US'}}));
assert.commandWorked(mongos.getDB(kDbName).foo.insert({a: 'foo'}));

jsTestLog('This index will inherit the collection\'s default collation.');
assert.commandWorked(mongos.getDB(kDbName).foo.createIndex({a: 1}));
assert.commandFailed(mongos.adminCommand(
    {shardCollection: kDbName + '.foo', key: {a: 1}, collation: {locale: 'simple'}}));

jsTestLog(
    'shardCollection should succeed on an empty collection with a non-simple default collation.');
mongos.getDB(kDbName).foo.drop();
assert.commandWorked(mongos.getDB(kDbName).createCollection('foo', {collation: {locale: 'en_US'}}));
assert.commandWorked(mongos.adminCommand(
    {shardCollection: kDbName + '.foo', key: {a: 1}, collation: {locale: 'simple'}}));

jsTestLog('shardCollection should succeed on an empty collection with no default collation.');
mongos.getDB(kDbName).foo.drop();
assert.commandWorked(mongos.getDB(kDbName).createCollection('foo'));
assert.commandWorked(mongos.adminCommand({shardCollection: kDbName + '.foo', key: {a: 1}}));

assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

//
// Tests for the shell helper sh.shardCollection().
//

db = mongos.getDB(kDbName);
assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));

jsTestLog('shardCollection() propagates the shard key and the correct defaults.');
mongos.getDB(kDbName).foo.drop();
assert.commandWorked(mongos.getDB(kDbName).createCollection('foo'));
assert.commandWorked(sh.shardCollection(kDbName + '.foo', {a: 1}));
indexSpec = getIndexSpecByName(mongos.getDB(kDbName).foo, 'a_1');
assert(!indexSpec.hasOwnProperty('unique'), tojson(indexSpec));
assert(!indexSpec.hasOwnProperty('collation'), tojson(indexSpec));

jsTestLog('shardCollection() propagates the value for "unique".');
mongos.getDB(kDbName).foo.drop();
assert.commandWorked(mongos.getDB(kDbName).createCollection('foo'));
assert.commandWorked(sh.shardCollection(kDbName + '.foo', {a: 1}, true));
indexSpec = getIndexSpecByName(mongos.getDB(kDbName).foo, 'a_1');
assert(indexSpec.hasOwnProperty('unique'), tojson(indexSpec));
assert.eq(indexSpec.unique, true, tojson(indexSpec));

mongos.getDB(kDbName).foo.drop();
assert.commandWorked(mongos.getDB(kDbName).createCollection('foo'));
assert.commandWorked(sh.shardCollection(kDbName + '.foo', {a: 1}, false));
indexSpec = getIndexSpecByName(mongos.getDB(kDbName).foo, 'a_1');
assert(!indexSpec.hasOwnProperty('unique'), tojson(indexSpec));

jsTestLog('shardCollections() "options" parameter must be an object.');
mongos.getDB(kDbName).foo.drop();
assert.commandWorked(mongos.getDB(kDbName).createCollection('foo'));
assert.throws(function() {
    sh.shardCollection(kDbName + '.foo', {a: 1}, false, 'not an object');
});

jsTestLog(
    'shardCollection() propagates the value for "collation". Currently only the simple collation is supported.');
mongos.getDB(kDbName).foo.drop();
assert.commandWorked(mongos.getDB(kDbName).createCollection('foo'));
assert.commandFailed(
    sh.shardCollection(kDbName + '.foo', {a: 1}, false, {collation: {locale: 'en_US'}}));
assert.commandWorked(
    sh.shardCollection(kDbName + '.foo', {a: 1}, false, {collation: {locale: 'simple'}}));
indexSpec = getIndexSpecByName(mongos.getDB(kDbName).foo, 'a_1');
assert(!indexSpec.hasOwnProperty('collation'), tojson(indexSpec));

mongos.getDB(kDbName).foo.drop();
assert.commandWorked(mongos.getDB(kDbName).createCollection('foo', {collation: {locale: 'en_US'}}));
assert.commandFailed(sh.shardCollection(kDbName + '.foo', {a: 1}));
assert.commandFailed(
    sh.shardCollection(kDbName + '.foo', {a: 1}, false, {collation: {locale: 'en_US'}}));
assert.commandWorked(
    sh.shardCollection(kDbName + '.foo', {a: 1}, false, {collation: {locale: 'simple'}}));
indexSpec = getIndexSpecByName(mongos.getDB(kDbName).foo, 'a_1');
assert(!indexSpec.hasOwnProperty('collation'), tojson(indexSpec));

jsTestLog('shardCollection() propagates the value for "numInitialChunks".');
mongos.getDB(kDbName).foo.drop();
assert.commandWorked(mongos.getDB(kDbName).createCollection('foo'));
assert.commandWorked(
    sh.shardCollection(kDbName + '.foo', {a: "hashed"}, false, {numInitialChunks: 5}));
st.printShardingStatus();
var numChunks = findChunksUtil.findChunksByNs(st.config, kDbName + '.foo').count();
assert.eq(numChunks, 5, "unexpected number of chunks");

st.stop();
})();
