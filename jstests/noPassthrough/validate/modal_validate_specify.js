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
    jsTestLog(validateLogs);
    assert.eq(1, validateLogs.length);
    const validateResult = validateLogs[0].attr.results;
    assert.eq("test.ham", validateResult.ns);
    assert.eq(2, validateResult.nIndexes);

    clearRawMongoProgramOutput();

    // Verify that command validates everything in the specified DB
    validateLogs = generateResults(dbpath, {validateDbName: "test"});
    jsTestLog("Specific DB");
    jsTestLog(validateLogs);
    assert.eq(2, validateLogs.length);
    const firstResult = validateLogs[0].attr.results;
    const secondResult = validateLogs[1].attr.results;
    assert(firstResult.ns == "test.ham" || firstResult.ns == "test.cheese");
    assert(secondResult.ns == "test.ham" || secondResult.ns == "test.cheese");
    assert.eq(2, firstResult.nIndexes);
    assert.eq(2, secondResult.nIndexes);
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
