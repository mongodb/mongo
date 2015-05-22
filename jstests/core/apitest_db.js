/**
 *   Tests for the db object enhancement
 */

assert( "test" == db, "wrong database currently not test" );

dd = function( x ){
    //print( x );
}

dd( "a" );


dd( "b" ); 

/*
 *  be sure the public collection API is complete
 */
assert(db.createCollection , "createCollection" );
assert(db.getProfilingLevel , "getProfilingLevel" );
assert(db.setProfilingLevel , "setProfilingLevel" );
assert(db.dbEval , "dbEval" );
assert(db.group , "group" );

dd( "c" );

/*
 * test createCollection
 */

db.getCollection( "test" ).drop();
db.getCollectionNames().forEach( function(x) { assert(x != "test"); });

dd( "d" );

db.createCollection("test");
var found = false;
db.getCollectionNames().forEach( function(x) { if (x == "test") found = true; });
assert(found, "found test.test in collection infos");

// storageEngine in collection options must:
// - be a document
// - all fields of the document:
// -- must have names that are registered storage engines
// -- must be objects
db.getCollection('test').drop();
var storageEngineName = db.serverStatus().storageEngine.name;
assert.commandFailed(db.createCollection('test', {storageEngine: 'not a document'}));
assert.commandWorked(db.createCollection('test', {storageEngine: {}}));
assert.commandFailed(db.createCollection('test', {storageEngine: {unknownStorageEngine: {}}}));
var invalidStorageEngineOptions = {}
invalidStorageEngineOptions[storageEngineName] = 12345;
assert.commandFailed(db.createCollection('test', {storageEngine: invalidStorageEngineOptions}));

// Test round trip of storageEngine in collection options.
// Assume that empty document for storageEngine-specific options is acceptable.
var validStorageEngineOptions = {}
validStorageEngineOptions[storageEngineName] = {};
db.getCollection('test').drop();
assert.commandWorked(db.createCollection('test', {storageEngine: validStorageEngineOptions}));

var collectionInfos = db.getCollectionInfos({name: 'test'});
assert.eq(1, collectionInfos.length, "'test' collection not created");
assert.eq('test', collectionInfos[0].name, "'test' collection not created");
assert.docEq(validStorageEngineOptions, collectionInfos[0].options.storageEngine,
             'storage engine options not found in listCommands result');

dd( "e" );

/*
 *  profile level
 */ 
 
db.setProfilingLevel(0);
assert(db.getProfilingLevel() == 0, "prof level 0");

db.setProfilingLevel(1);
assert(db.getProfilingLevel() == 1, "p1");

db.setProfilingLevel(2);
assert(db.getProfilingLevel() == 2, "p2");

db.setProfilingLevel(0);
assert(db.getProfilingLevel() == 0, "prof level 0");

dd( "f" );
asserted = false;
try {
    db.setProfilingLevel(10);
    assert(false);
}
catch (e) { 
    asserted = true;
    assert(e.dbSetProfilingException);
}
assert( asserted, "should have asserted" );

dd( "g" );



assert.eq( "foo" , db.getSisterDB( "foo" ).getName() )
assert.eq( "foo" , db.getSiblingDB( "foo" ).getName() )

