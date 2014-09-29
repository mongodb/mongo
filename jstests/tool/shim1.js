// Mongoshim is a tool to read/write the stored format directly.
// Intended to be used by other mongo tools that support the --dbpath option
// to access the data directory in the absence of a running mongod server.
//
// Mongoshim runs in a few modes:
//
// --mode=find (default):
//     Reads contents of a collection and writes the documents in BSON format to stdout.
//     Documents may be filtered using the --query option.
//     Another mongo tool "bsondump" may be used to convert the written BSON to a human-readable
//     format.
//
// --mode=insert:
//     Reads BSON documents from stdin and inserts them in a collection.
//     Input documents may be filtered using the --filter option.
//
// --mode=upsert:
//     A variation of "insert" where existing documents will be updated with
//     the BSON data read from stdin. The existing documents are identified
//     by matching the _id field or fields specified in --upsertFields.
//
// --mode=remove:
//     Removes documents from an existing collection matching the predicate in --filter.
//     If --filter is not specified, all documents will be removed from the collection.
//
// --mode=repair:
//     Reads record store for collection. Outputs valid documents.
//
//  --mode=applyOps:
//     Applies oplog entries using the applyOps command. BSON documents read from stdin will
//     be used as the list of operations to be applied to the oplog.

var baseName = 'jstests_tool_shim1';
var dbPath = MongoRunner.dataPath + baseName + '/';
var externalPath = MongoRunner.dataPath + baseName + '_external/';
var externalBaseName = 'shim1.bson';
var externalFile = externalPath + externalBaseName;
var externalOperationsBaseName = 'shim1_operations.bson';
var externalOperationsFile = externalPath + externalOperationsBaseName;

var dbName = 'test';
var collectionName = 'shim1';
var operationsCollectionName = 'shim1operations';

var port = allocatePorts(1)[0];

resetDbpath(dbPath);
resetDbpath(externalPath);

// Queries filesystem for size of 'externalFile' in bytes.
// Returns -1 if file is not found.
function fileSize(baseName){
    var files = listFiles(externalPath);
    for (var i=0; i<files.length; i++) {
        if (files[i].baseName == baseName)
            return files[i].size;
    }
    assert(false, 'BSON file not found: ' + externalFile);
}

// Starts mongod server without wiping out data directory
function startServer() {
    return startMongoProgram('mongod', '--port', port, '--dbpath', dbPath,
                             '--nohttpinterface', '--noprealloc', '--bind_ip', '127.0.0.1');
}

// Stops mongod server.
function stopServer() {
    stopMongod(port);
}

//
// Invalid command line option and mode combinations
//

// Missing --collection option.
assert.neq(0, runMongoProgram('mongoshim', '--dbpath', dbPath,
                              '--db', dbName,
                              '--out', externalFile),
           'expected non-zero error code when required --collection option is omitted');



//
// Basic tests for each mode.
//

// Initialize collection.
var mongod = startServer();
var collection = mongod.getDB(dbName).getCollection(collectionName);
collection.save({a: 1});
var doc = collection.findOne();
stopServer();

// "read" mode - read collection with single document and write results to file.
// XXX: Do not check tool exit code. See SERVER-5520
runMongoProgram('mongoshim', '--dbpath', dbPath,
                '--db', dbName, '--collection', collectionName,
                '--out', externalFile);
assert.eq(Object.bsonsize(doc), fileSize(externalBaseName),
          'output BSON file size does not match size of document returned from find()');

// "insert" mode - insert document from file into collection.
resetDbpath(dbPath);
runMongoProgram('mongoshim', '--dbpath', dbPath,
                '--db', dbName, '--collection', collectionName,
                '--mode', 'insert',
                '--in', externalFile);
mongod = startServer();
collection = mongod.getDB(dbName).getCollection(collectionName);
assert.eq(1, collection.count(), 'test document was not added to collection');
var newDoc = collection.findOne();
assert.eq(doc, newDoc, 'invalid document saved by mongoshim --mode=insert')
stopServer();

// "upsert" mode - upsert document from file into collection.
// Since document already exists in collection, this upsert operation will
// have no effect.
runMongoProgram('mongoshim', '--dbpath', dbPath,
                '--db', dbName, '--collection', collectionName,
                '--mode', 'upsert',
                '--upsertFields', 'a',
                '--inputDocuments', tojson({in: [{a: 1, b: 1}]}));
mongod = startServer();
collection = mongod.getDB(dbName).getCollection(collectionName);
assert.eq(1, collection.count(), 'test document was not added to collection');
var newDoc = collection.findOne({}, {_id: 0, a: 1, b: 1});
assert.eq({a: 1, b: 1}, newDoc, 'invalid document saved by mongoshim --mode=upsert')
stopServer();

// "remove" mode - remove documents from collection.
resetDbpath(dbPath);
runMongoProgram('mongoshim', '--dbpath', dbPath,
                '--db', dbName, '--collection', collectionName,
                '--mode', 'remove');
mongod = startServer();
collection = mongod.getDB(dbName).getCollection(collectionName);
assert.eq(0, collection.count(), 'test document was not removed from collection');
stopServer();

// "applyOps" mode - apply oplog entries to collection.
// Write BSON file containing operation to be applied.
mongod = startServer();
collection = mongod.getDB(dbName).getCollection(collectionName);
var collectionFullName = collection.getFullName();
var operationsCollection = mongod.getDB(dbName).getCollection(operationsCollectionName);
operationsCollection.save({op: 'i', ns: collectionFullName, o: {_id: 1, a: 1 }});
collection.drop();
stopServer();
runMongoProgram('mongoshim', '--dbpath', dbPath,
                '--db', dbName, '--collection', operationsCollectionName,
                '--out', externalOperationsFile);
// Apply operations in BSON file.
// If operation was applied successfully, we should see a document in
// 'collectionName' collection (not 'operationsCollectionName').
runMongoProgram('mongoshim', '--dbpath', dbPath,
                '--db', dbName, '--collection', operationsCollectionName,
                '--mode', 'applyOps',
                '--in', externalOperationsFile);
mongod = startServer();
collection = mongod.getDB(dbName).getCollection(collectionName);
assert.eq({_id: 1, a: 1 }, collection.findOne(),
          'mongoshim failed to apply operaton to ' + collectionName);
stopServer();
