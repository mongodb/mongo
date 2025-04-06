/**
 * Tests that the '--validate' takes in a specific database / collection and validates it modally.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 * ]
 */
function setupCollections(db) {
    assert.commandWorked(db.createCollection('ham'));
    assert.commandWorked(db['ham'].insert({a: 1}));
    assert.commandWorked(db['ham'].createIndex({b: 1}));

    assert.commandWorked(db.createCollection('cheese'));
    assert.commandWorked(db['cheese'].insert({a: 1}));
    assert.commandWorked(db['cheese'].createIndex({b: 1}));
}

function generateResults(dbpath, opts) {
    MongoRunner.runMongod({dbpath: dbpath, validate: "", setParameter: opts, noCleanData: true});
    let validateLogs = rawMongoProgramOutput("(9437301)")
                           .split("\n")
                           .filter(line => line.trim() !== "")
                           .map(line => JSON.parse(line.split("|").slice(1).join("|")));
    return validateLogs;
}

function runDbTest() {
    // Setup the dbpath for this test.
    const dbpath = MongoRunner.dataPath + 'modal_validate_specify';

    // Setup DBs and collections
    let conn = MongoRunner.runMongod({dbpath: dbpath});
    const port = conn.port;
    let testDB = conn.getDB("test");
    let adminDB = conn.getDB("admin");
    setupCollections(testDB);
    setupCollections(adminDB);

    MongoRunner.stopMongod(conn);
    clearRawMongoProgramOutput();

    // Verify that command validates everything in the specified collection
    let validateLogs =
        generateResults(dbpath, {validateDbName: "test", validateCollectionName: "ham"});
    jsTestLog("Specific Collection");
    assert.eq(1, validateLogs.length);
    assert.eq("test.ham", validateLogs[0].attr.results.ns);
    jsTestLog(validateLogs);
    clearRawMongoProgramOutput();

    // Verify that command validates everything in the specified DB
    validateLogs = generateResults(dbpath, {validateDbName: "test"});
    jsTestLog("Specific DB");
    assert.eq(2, validateLogs.length);
    assert(validateLogs[0].attr.results.ns == "test.ham" ||
           validateLogs[0].attr.results.ns == "test.cheese");
    assert(validateLogs[1].attr.results.ns == "test.ham" ||
           validateLogs[1].attr.results.ns == "test.cheese");
    jsTestLog(validateLogs);
    clearRawMongoProgramOutput();

    // Error if collection is non-existant
    assert.neq(MongoRunner.EXIT_CLEAN,
               runMongoProgram("mongod",
                               "--validate",
                               "--port",
                               port,
                               "--dbpath",
                               dbpath,
                               "--setParameter",
                               "validateDbName=test",
                               "--setParameter",
                               "validateCollectionName=lettuce"));
}

runDbTest();
