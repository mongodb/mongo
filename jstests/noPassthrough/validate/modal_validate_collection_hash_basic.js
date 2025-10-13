/**
 * Tests that the '--validate' takes in collection hash options.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 * ]
 */

const dbpath = MongoRunner.dataPath + jsTestName();
const dbName = jsTestName();
const collName = jsTestName();

function runValidate(opts) {
    jsTest.log.info(`Running validate with ${tojson(opts)}`);
    MongoRunner.runMongod({
        dbpath: dbpath,
        validate: "",
        setParameter: {
            validateDbName: dbName,
            validateCollectionName: collName,
            collectionValidateOptions: {options: opts},
        },
        noCleanData: true,
    });
    const validateResults = rawMongoProgramOutput("(9437301)")
        .split("\n")
        .filter((line) => line.trim() !== "")
        .map((line) => JSON.parse(line.split("|").slice(1).join("|")));
    assert.eq(validateResults.length, 1);
    jsTest.log.info(`Validate result with ${tojson(opts)}:\n${tojson(validateResults[0])}`);
    clearRawMongoProgramOutput();
    return validateResults[0].attr.results;
}

function assertValidateFails(opts) {
    jsTest.log.info(`Running validate with ${tojson(opts)} and expecting to fail`);
    assert.neq(
        MongoRunner.EXIT_CLEAN,
        runMongoProgram(
            "mongod",
            "--validate",
            "--port",
            port,
            "--dbpath",
            dbpath,
            "--setParameter",
            `validateDbName=${dbName}`,
            "--setParameter",
            `validateCollectionName=${collName}`,
            "--setParameter",
            `collectionValidateOptions={options: ${tojson(opts)}}`,
        ),
    );
}

// Set up the collection and shut down the server.
const conn = MongoRunner.runMongod({dbpath: dbpath});
const port = conn.port;
const db = conn.getDB(dbName);
const coll = db.getCollection(collName);

assert.commandWorked(coll.insert({a: 1}));

MongoRunner.stopMongod(conn);

// Verify the modal validate fails when options are invalid.
assertValidateFails({hashPrefixes: []});
assertValidateFails({collHash: true, hashPrefixes: [3]});
assertValidateFails({collHash: true, revealHashedIds: []});
assertValidateFails({collHash: true, hashPrefixes: [], revealHashedIds: []});

// Verify basic collection hash validate results are expected for modal validate.
let res = runValidate({collHash: true});
assert(res.all);
assert(res.metadata);

res = runValidate({collHash: true, hashPrefixes: []});
assert(res.partial);
const hashPrefix = Object.keys(res.partial)[0];

res = runValidate({collHash: true, revealHashedIds: [hashPrefix]});
assert(res.all);
assert(res.metadata);
assert(res.revealedIds);
