/**
 * Test shardCollection command behavior
 */

import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

var kDbName = db.getName()

db.dropDatabase();

function testAndClenaupWithKeyNoIndexFailed(keyDoc) {
    assert.commandWorked(db.adminCommand({enableSharding: kDbName}));

    var ns = db.getName() + '.foo';
    assert.commandFailed(db.adminCommand({shardCollection: ns, key: keyDoc}));

    assert.eq(
        db.getSiblingDB('config').collections.countDocuments({_id: ns, unsplittable: {$ne: true}}),
        0);
    assert.commandWorked(db.dropDatabase());
}

function testAndClenaupWithKeyOK(keyDoc) {
    assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
    assert.commandWorked(db.foo.createIndex(keyDoc));

    var ns = kDbName + '.foo';
    assert.eq(
        db.getSiblingDB('config').collections.countDocuments({_id: ns, unsplittable: {$ne: true}}),
        0);

    assert.commandWorked(db.adminCommand({shardCollection: ns, key: keyDoc}));

    assert.eq(
        db.getSiblingDB('config').collections.countDocuments({_id: ns, unsplittable: {$ne: true}}),
        1);
    assert.commandWorked(db.dropDatabase());
}

function testAndClenaupWithKeyNoIndexOK(keyDoc) {
    assert.commandWorked(db.adminCommand({enableSharding: kDbName}));

    var ns = kDbName + '.foo';
    assert.eq(
        db.getSiblingDB('config').collections.countDocuments({_id: ns, unsplittable: {$ne: true}}),
        0);

    assert.commandWorked(db.adminCommand({shardCollection: ns, key: keyDoc}));

    assert.eq(
        db.getSiblingDB('config').collections.countDocuments({_id: ns, unsplittable: {$ne: true}}),
        1);
    assert.commandWorked(db.dropDatabase());
}

function getIndexSpecByName(coll, indexName) {
    var indexes = coll.getIndexes().filter(function(spec) {
        return spec.name === indexName;
    });
    assert.eq(1, indexes.length, 'index "' + indexName + '" not found"');
    return indexes[0];
}

assert.commandWorked(db.foo.insert({a: 1, b: 1}));
assert.commandWorked(db.adminCommand({enableSharding: kDbName}));

{
    jsTestLog('Verify wrong arguments errors.');
    assert.commandFailed(db.adminCommand({shardCollection: 'foo', key: {_id: 1}}));

    assert.commandFailed(db.adminCommand({shardCollection: kDbName + '.foo', key: "aaa"}));

    jsTestLog('Verify namespace length limit.');

    const longEnoughNs = kDbName + '.' +
        'x'.repeat(235 - kDbName.length - 1);
    assert.commandWorked(db.adminCommand({shardCollection: longEnoughNs, key: {_id: 1}}));

    const tooLongNs = longEnoughNs + 'x';
    assert.commandFailedWithCode(db.adminCommand({shardCollection: tooLongNs, key: {_id: 1}}),
                                 ErrorCodes.InvalidNamespace);
}

{
    jsTestLog("Special collections can't be sharded");

    let specialColls = [
        'config.foo',  // all collections in config db except config.system.sessions
        'admin.foo',   // all collections is admin db can't be sharded
    ];

    specialColls.forEach(collName => {
        assert.commandFailedWithCode(db.adminCommand({shardCollection: collName, key: {_id: 1}}),
                                     ErrorCodes.IllegalOperation);
    });

    // System collections can't be sharded
    // If we are running in suites with authentication enabled we do expect a different error
    const expectedErrorCode = TestData.auth ? ErrorCodes.Unauthorized : ErrorCodes.IllegalOperation;
    assert.commandFailedWithCode(
        db.adminCommand({shardCollection: `${kDbName}.system.foo`, key: {_id: 1}}),
        expectedErrorCode);

    // For collections in the local database the router will attempt to create the db and it will
    // fail with InvalidOptions
    assert.commandFailedWithCode(db.adminCommand({shardCollection: 'local.foo', key: {_id: 1}}),
                                 ErrorCodes.InvalidOptions);
}

jsTestLog('shardCollection may only be run against admin database.');
assert.commandFailed(db.runCommand({shardCollection: kDbName + '.foo', key: {_id: 1}}));

assert.commandWorked(db.foo.insert({a: 1, b: 1}));

jsTestLog('Cannot shard if key is not specified.');
assert.commandFailed(db.adminCommand({shardCollection: kDbName + '.foo'}));

assert.commandFailed(db.adminCommand({shardCollection: kDbName + '.foo', key: {}}));

jsTestLog('Verify key format.');
assert.commandFailed(
    db.adminCommand({shardCollection: kDbName + '.foo', key: {aKey: 'hahahashed'}}));

jsTestLog('Shard key cannot contain embedded objects.');
assert.commandFailed(db.adminCommand({shardCollection: kDbName + '.foo', key: {_id: {a: 1}}}));
assert.commandFailed(db.adminCommand({shardCollection: kDbName + '.foo', key: {_id: {'a.b': 1}}}));

jsTestLog('Shard key can contain dotted path to embedded element.');
assert.commandWorked(
    db.adminCommand({shardCollection: kDbName + '.shard_key_dotted_path', key: {'_id.a': 1}}));

jsTestLog('Command should still verify index even if implicitlyCreateIndex is false.');
assert.commandFailedWithCode(
    db.adminCommand({shardCollection: kDbName + '.foo', key: {x: 1}, implicitlyCreateIndex: false}),
    6373200);

//
// Test shardCollection's idempotency
//

jsTestLog('Succeed if a collection is already sharded with the same options.');
assert.commandWorked(db.adminCommand({shardCollection: kDbName + '.foo', key: {_id: 1}}));
assert.commandWorked(db.adminCommand({shardCollection: kDbName + '.foo', key: {_id: 1}}));

jsTestLog(
    'Specifying the simple collation or not specifying a collation should be equivalent, because if no collation is specified, the collection default collation is used.');
assert.commandWorked(db.adminCommand(
    {shardCollection: kDbName + '.foo', key: {_id: 1}, collation: {locale: 'simple'}}));

jsTestLog('Fail if the collection is already sharded with different options. different shard key');
assert.commandFailed(db.adminCommand({shardCollection: kDbName + '.foo', key: {x: 1}}));

jsTestLog('Different "unique"');
assert.commandFailed(
    db.adminCommand({shardCollection: kDbName + '.foo', key: {_id: 1}, unique: true}));

assert.commandWorked(db.dropDatabase());

jsTestLog('Allow non-unique index if enforceUniquenessCheck is false');
assert.commandWorked(db.foo.createIndex({x: 1}));
assert.commandWorked(db.adminCommand(
    {shardCollection: kDbName + '.foo', key: {x: 1}, unique: true, enforceUniquenessCheck: false}));
let collDoc = db.getSiblingDB('config').collections.findOne({_id: `${kDbName}.foo`});
assert(collDoc);
assert(collDoc.unique);
assert.commandWorked(db.dropDatabase());

jsTestLog('dbync unique key pattern use case');
assert.commandWorked(db.foo.createIndex({x: 1}));
assert.commandWorked(db.adminCommand({
    shardCollection: kDbName + '.foo',
    key: {x: 1},
    unique: true,
    implicitlyCreateIndex: false,
    enforceUniquenessCheck: false
}));
collDoc = db.getSiblingDB('config').collections.findOne({_id: `${kDbName}.foo`});
assert(collDoc);
assert(collDoc.unique);
assert.commandWorked(db.dropDatabase());

jsTestLog('Shard empty collections no index required.');
testAndClenaupWithKeyNoIndexOK({_id: 1});
testAndClenaupWithKeyNoIndexOK({_id: 'hashed'});

jsTestLog('Shard by a plain key.');
testAndClenaupWithKeyNoIndexOK({a: 1});

jsTestLog('Cant shard collection with data and no index on the shard key.');
assert.commandWorked(db.foo.insert({a: 1, b: 1}));
testAndClenaupWithKeyNoIndexFailed({a: 1});

assert.commandWorked(db.foo.insert({a: 1, b: 1}));
testAndClenaupWithKeyOK({a: 1});

jsTestLog('Shard by a hashed key.');
testAndClenaupWithKeyNoIndexOK({a: 'hashed'});

assert.commandWorked(db.foo.insert({a: 1, b: 1}));
testAndClenaupWithKeyNoIndexFailed({a: 'hashed'});

assert.commandWorked(db.foo.insert({a: 1, b: 1}));
testAndClenaupWithKeyOK({a: 'hashed'});

jsTestLog('Shard by a compound key.');
testAndClenaupWithKeyNoIndexOK({x: 1, y: 1});

assert.commandWorked(db.foo.insert({x: 1, y: 1}));
testAndClenaupWithKeyNoIndexFailed({x: 1, y: 1});

assert.commandWorked(db.foo.insert({x: 1, y: 1}));
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
assert.commandWorked(db.foo.createIndex({a: 1}));
assert.commandWorked(db.foo.insert({a: [1, 2, 3, 4, 5], b: 1}));
testAndClenaupWithKeyNoIndexFailed({a: 1});

assert.commandWorked(db.foo.createIndex({a: 1, b: 1}));
assert.commandWorked(db.foo.insert({a: [1, 2, 3, 4, 5], b: 1}));
testAndClenaupWithKeyNoIndexFailed({a: 1, b: 1});

assert.commandWorked(db.foo.insert({a: 1, b: 1}));
testAndClenaupWithKeyNoIndexFailed({a: 'hashed'});

assert.commandWorked(db.foo.insert({a: 1, b: 1}));
testAndClenaupWithKeyOK({a: 'hashed'});

jsTestLog('Cannot shard by a parallel arrays.');
assert.commandWorked(db.foo.insert({a: [1, 2, 3, 4, 5], b: [1, 2, 3, 4, 5]}));
testAndClenaupWithKeyNoIndexFailed({a: 1, b: 1});

assert.commandWorked(db.adminCommand({enableSharding: kDbName}));

jsTestLog('Cannot shard on unique hashed key.');
assert.commandFailed(
    db.adminCommand({shardCollection: kDbName + '.foo', key: {aKey: "hashed"}, unique: true}));

jsTestLog('If shardCollection has unique:true it  must have a unique index.');
assert.commandWorked(db.foo.createIndex({aKey: 1}));

assert.commandFailed(
    db.adminCommand({shardCollection: kDbName + '.foo', key: {aKey: 1}, unique: true}));

//
// Session-related tests
//

assert.commandWorked(db.dropDatabase());
assert.commandWorked(db.adminCommand({enableSharding: kDbName}));

jsTestLog('shardCollection can be called under a session.');
const sessionDb = db.getMongo().startSession().getDatabase(kDbName);
assert.commandWorked(
    sessionDb.adminCommand({shardCollection: kDbName + '.foo', key: {_id: 'hashed'}}));
sessionDb.getSession().endSession();

assert.commandWorked(db.dropDatabase());

//
// Collation-related tests
//

assert.commandWorked(db.dropDatabase());
assert.commandWorked(db.adminCommand({enableSharding: kDbName}));

jsTestLog('shardCollection should fail when the "collation" option is not a nested object.');
assert.commandFailed(
    db.adminCommand({shardCollection: kDbName + '.foo', key: {_id: 1}, collation: true}));

jsTestLog('shardCollection should fail when the "collation" option cannot be parsed.');
assert.commandFailed(db.adminCommand(
    {shardCollection: kDbName + '.foo', key: {_id: 1}, collation: {locale: 'unknown'}}));

jsTestLog(
    'shardCollection should fail when the "collation" option is valid but is not the simple collation.');
assert.commandFailed(db.adminCommand(
    {shardCollection: kDbName + '.foo', key: {_id: 1}, collation: {locale: 'en_US'}}));

jsTestLog(
    'shardCollection should succeed when the "collation" option specifies the simple collation.');
assert.commandWorked(db.adminCommand(
    {shardCollection: kDbName + '.foo', key: {_id: 1}, collation: {locale: 'simple'}}));

jsTestLog(
    'shardCollection should fail when it does not specify the "collation" option but the collection has a non-simple default collation.');
db.foo.drop();
assert.commandWorked(db.createCollection('foo', {collation: {locale: 'en_US'}}));
assert.commandFailed(db.adminCommand({shardCollection: kDbName + '.foo', key: {a: 1}}));

jsTestLog(
    'shardCollection should fail for the key pattern {_id: 1} if the collection has a non-simple default collation.');
db.foo.drop();
assert.commandWorked(db.createCollection('foo', {collation: {locale: 'en_US'}}));
assert.commandFailed(db.adminCommand(
    {shardCollection: kDbName + '.foo', key: {_id: 1}, collation: {locale: 'simple'}}));

jsTestLog(
    'shardCollection should fail for the key pattern {a: 1} if there is already an index "a_1", but it has a non-simple collation.');
db.foo.drop();
assert.commandWorked(db.foo.createIndex({a: 1}, {collation: {locale: 'en_US'}}));
assert.commandFailed(db.adminCommand({shardCollection: kDbName + '.foo', key: {a: 1}}));

jsTestLog(
    'shardCollection should succeed for the key pattern {a: 1} and collation {locale: "simple"} if there is no index "a_1", but there is a non-simple collection default collation.');
db.foo.drop();
assert.commandWorked(db.createCollection('foo', {collation: {locale: 'en_US'}}));
assert.commandWorked(db.adminCommand(
    {shardCollection: kDbName + '.foo', key: {a: 1}, collation: {locale: 'simple'}}));
var indexSpec = getIndexSpecByName(db.foo, 'a_1');
assert(!indexSpec.hasOwnProperty('collation'));

jsTestLog(
    'shardCollection should succeed for the key pattern {a: 1} if there are two indexes on {a: 1} and one has the simple collation.');
db.foo.drop();
assert.commandWorked(db.foo.createIndex({a: 1}, {name: "a_1_simple"}));
assert.commandWorked(db.foo.createIndex({a: 1}, {collation: {locale: 'en_US'}, name: "a_1_en_US"}));
assert.commandWorked(db.adminCommand({shardCollection: kDbName + '.foo', key: {a: 1}}));

jsTestLog(
    'shardCollection should fail on a non-empty collection when the only index available with the shard key as a prefix has a non-simple collation.');
db.foo.drop();
assert.commandWorked(db.createCollection('foo', {collation: {locale: 'en_US'}}));
assert.commandWorked(db.foo.insert({a: 'foo'}));

jsTestLog('This index will inherit the collection\'s default collation.');
assert.commandWorked(db.foo.createIndex({a: 1}));
assert.commandFailed(db.adminCommand(
    {shardCollection: kDbName + '.foo', key: {a: 1}, collation: {locale: 'simple'}}));

jsTestLog(
    'shardCollection should succeed on an empty collection with a non-simple default collation.');
db.foo.drop();
assert.commandWorked(db.createCollection('foo', {collation: {locale: 'en_US'}}));
assert.commandWorked(db.adminCommand(
    {shardCollection: kDbName + '.foo', key: {a: 1}, collation: {locale: 'simple'}}));

jsTestLog('shardCollection should succeed on an empty collection with no default collation.');
db.foo.drop();
assert.commandWorked(db.createCollection('foo'));
assert.commandWorked(db.adminCommand({shardCollection: kDbName + '.foo', key: {a: 1}}));

assert.commandWorked(db.dropDatabase());

//
// Tests for the shell helper sh.shardCollection().
//

globalThis.db = db;
assert.commandWorked(db.adminCommand({enableSharding: kDbName}));

jsTestLog('shardCollection() propagates the shard key and the correct defaults.');
db.foo.drop();
assert.commandWorked(db.createCollection('foo'));
assert.commandWorked(sh.shardCollection(kDbName + '.foo', {a: 1}));
indexSpec = getIndexSpecByName(db.foo, 'a_1');
assert(!indexSpec.hasOwnProperty('unique'), tojson(indexSpec));
assert(!indexSpec.hasOwnProperty('collation'), tojson(indexSpec));

jsTestLog('shardCollection() propagates the value for "unique".');
db.foo.drop();
assert.commandWorked(db.createCollection('foo'));
assert.commandWorked(sh.shardCollection(kDbName + '.foo', {a: 1}, true));
indexSpec = getIndexSpecByName(db.foo, 'a_1');
assert(indexSpec.hasOwnProperty('unique'), tojson(indexSpec));
assert.eq(indexSpec.unique, true, tojson(indexSpec));

db.foo.drop();
assert.commandWorked(db.createCollection('foo'));
assert.commandWorked(sh.shardCollection(kDbName + '.foo', {a: 1}, false));
indexSpec = getIndexSpecByName(db.foo, 'a_1');
assert(!indexSpec.hasOwnProperty('unique'), tojson(indexSpec));

jsTestLog('shardCollections() "options" parameter must be an object.');
db.foo.drop();
assert.commandWorked(db.createCollection('foo'));
assert.throws(function() {
    sh.shardCollection(kDbName + '.foo', {a: 1}, false, 'not an object');
});

jsTestLog(
    'shardCollection() propagates the value for "collation". Currently only the simple collation is supported.');
db.foo.drop();
assert.commandWorked(db.createCollection('foo'));
assert.commandFailed(
    sh.shardCollection(kDbName + '.foo', {a: 1}, false, {collation: {locale: 'en_US'}}));
assert.commandWorked(
    sh.shardCollection(kDbName + '.foo', {a: 1}, false, {collation: {locale: 'simple'}}));
indexSpec = getIndexSpecByName(db.foo, 'a_1');
assert(!indexSpec.hasOwnProperty('collation'), tojson(indexSpec));

db.foo.drop();
assert.commandWorked(db.createCollection('foo', {collation: {locale: 'en_US'}}));
assert.commandFailed(sh.shardCollection(kDbName + '.foo', {a: 1}));
assert.commandFailed(
    sh.shardCollection(kDbName + '.foo', {a: 1}, false, {collation: {locale: 'en_US'}}));
assert.commandWorked(
    sh.shardCollection(kDbName + '.foo', {a: 1}, false, {collation: {locale: 'simple'}}));
indexSpec = getIndexSpecByName(db.foo, 'a_1');
assert(!indexSpec.hasOwnProperty('collation'), tojson(indexSpec));
