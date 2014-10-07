// Mongoshim is a tool to read/write the stored format directly.
// Intended to be used by other mongo tools that support the --dbpath option
// to access the data directory in the absence of a running mongod server.
//
// Mongoshim runs in a few modes:
//
// --mode=find:
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
//  --mode=command:
//     Invokes db.runCommand() using command objects read from input.

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

// Missing --mode option.
assert.neq(0, runMongoProgram('mongoshim', '--dbpath', dbPath,
                              '--db', dbName,
                              '--collection', collectionName),
           'expected non-zero error code when required --mode option is omitted');

// Missing --dbpath option.
assert.neq(0, runMongoProgram('mongoshim',
                              '--db', dbName,
                              '--collection', collectionName,
                              '--mode', 'find'),
           'expected non-zero error code when required --dbpath option is omitted');

// Missing --collection option when mode is not "command".
assert.neq(0, runMongoProgram('mongoshim', '--dbpath', dbPath,
                              '--db', dbName,
                              '--mode', 'find'),
           'expected non-zero error code when required --collection option is omitted');

// --collection and --mode=command are incompatible.
assert.neq(0, runMongoProgram('mongoshim', '--dbpath', dbPath,
                              '--db', dbName, '--collection', collectionName,
                              '--mode', 'command'),
           'expected non-zero error code when incompatible options --collection and ' +
           '--mode=command are used');

// --drop cannot be used with non-insert mode.
assert.neq(0, runMongoProgram('mongoshim', '--dbpath', dbPath,
                              '--db', dbName,
                              '--mode', 'command',
                              '--drop'),
           'expected non-zero error code when incompatible options --drop and ' +
           '--mode=command are used');

// --upsertFields cannot be used with non-upsert mode.
assert.neq(0, runMongoProgram('mongoshim', '--dbpath', dbPath,
                              '--db', dbName, '--collection', collectionName,
                              '--mode', 'insert',
                              '--upsertFields', 'a,b,c'),
           'expected non-zero error code when incompatible options --upsertFields and ' +
           '--mode=insert are used');

//
// Basic tests for each mode.
//

// Initialize collection.
var mongod = startServer();
var collection = mongod.getDB(dbName).getCollection(collectionName);
collection.save({a: 1});
var doc = collection.findOne();
stopServer();

// "find" mode - read collection with single document and write results to file.
// XXX: Do not check tool exit code. See SERVER-5520
runMongoProgram('mongoshim', '--dbpath', dbPath,
                '--db', dbName, '--collection', collectionName,
                '--mode', 'find',
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
// drop collection before inserting new document.
resetDbpath(dbPath);
runMongoProgram('mongoshim', '--dbpath', dbPath,
                '--db', dbName, '--collection', collectionName,
                '--mode', 'insert',
                '--drop',
                '--inputDocuments', tojson({in: [{a: 1, c: 1}]}));
mongod = startServer();
collection = mongod.getDB(dbName).getCollection(collectionName);
assert.eq(1, collection.count(), 'collection was not dropped before insertion');
var newDoc = collection.findOne();
assert.eq(1, newDoc.c, 'invalid document saved by mongoshim --mode=insert')
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
// Simulated using "command" mode.
// If operation was applied successfully, we should see a document in
// 'collectionName' collection (not 'operationsCollectionName').
var collectionFullName = dbName + '.' + collectionName;
runMongoProgram('mongoshim', '--dbpath', dbPath,
                '--db', dbName,
                '--mode', 'command',
                '--inputDocuments',
                tojson({in: [{applyOps: [
                    {op: 'i', ns: collectionFullName, o: {_id: 1, a: 1}}]}]}));
mongod = startServer();
collection = mongod.getDB(dbName).getCollection(collectionName);
assert.eq({_id: 1, a: 1 }, collection.findOne(),
          'mongoshim failed to apply operaton to ' + collectionName);
stopServer();

// "command" mode - invoke db.runCommand with command object read from input.
// Command result written to output.
// First "command" test case uses the "ping" command and saves the command
// result into the test collection for verification.
// Collection parameter is ignored in "command" mode.
runMongoProgram('mongoshim', '--dbpath', dbPath,
                '--db', dbName,
                '--mode', 'command',
                '--inputDocuments', tojson({in: [{ping: 1}]}),
                '--out', externalFile);
runMongoProgram('mongoshim', '--dbpath', dbPath,
                '--db', dbName, '--collection', collectionName,
                '--mode', 'insert',
                '--drop',
                '--in', externalFile);
mongod = startServer();
collection = mongod.getDB(dbName).getCollection(collectionName);
assert.commandWorked(collection.findOne(),
                     'mongoshim failed to run "ping" command');
stopServer();
// Second "command" test case runs a non-existent command.
runMongoProgram('mongoshim', '--dbpath', dbPath,
                '--db', dbName,
                '--mode', 'command',
                '--inputDocuments', tojson({in: [{noSuchCommand: 1}]}),
                '--out', externalFile);
runMongoProgram('mongoshim', '--dbpath', dbPath,
                '--db', dbName, '--collection', collectionName,
                '--mode', 'insert',
                '--drop',
                '--in', externalFile);
mongod = startServer();
collection = mongod.getDB(dbName).getCollection(collectionName);
assert.commandFailed(collection.findOne(),
                     'mongoshim should get a failed status from running "noSuchCommand" command');
stopServer();
