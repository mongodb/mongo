/**
 * Tests that modal validation at 'atClusterTime' gracefully skips collections that are present in
 * the latest catalog but did not exist at 'atClusterTime', e.g. collections created or renamed into
 * place afterwards. Such collections should be reported as valid with a warning rather than failing
 * validation and preventing startup. Also tests startup validation for a single collection created after 'atClusterTime', the command
 * should return non-OK status to indicate that the collection was not validated.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 *   requires_replication,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {parseValidateOutputsFromLogs} from "jstests/noPassthrough/validate/libs/validate_find_repl_set_divergence.js";

const dbpath = MongoRunner.dataPath + jsTestName();

const rst = new ReplSetTest({nodes: 1});
rst.startSet({dbpath: dbpath});
rst.initiate();

const dbName = jsTestName();
const primary = rst.getPrimary();
const db = primary.getDB(dbName);

// Collections that exist at 'atClusterTime'.
assert.commandWorked(db.runCommand({insert: "existing", documents: [{a: 1}]}));
const res = assert.commandWorked(db.runCommand({insert: "toRename", documents: [{a: 1}]}));

// Capture a cluster time at which only 'existing' and 'toRename' exist.
const atClusterTime = res.operationTime;

// Create and rename after 'atClusterTime': 'late' and 'renamed' are absent then but present now.
assert.commandWorked(db.runCommand({insert: "late", documents: [{a: 1}]}));
assert.commandWorked(
    db.adminCommand({renameCollection: `${dbName}.toRename`, to: `${dbName}.renamed`}),
);

rst.stopSet(null /* signal */, true /* forRestart */);

// Run modal validation at 'atClusterTime'. If absent collections aren't skipped, mongod exits
// non-zero and runMongod throws.
MongoRunner.runMongod({
    dbpath: dbpath,
    validate: "",
    setParameter: {collectionValidateOptions: {options: {atClusterTime: atClusterTime}}},
    noCleanData: true,
});

const validateResults = parseValidateOutputsFromLogs();
jsTest.log.info(`Validate result:\n${tojson(validateResults)}`);

const resultsByNs = {};
validateResults.forEach((result) => {
    const r = result.attr.results;
    assert(r.valid, `Expected valid result for ${r.ns}: ${tojson(result)}`);
    resultsByNs[r.ns] = r;
});

// 'existing' existed at 'atClusterTime' and should be validated normally.
assert(
    resultsByNs[`${dbName}.existing`],
    `missing result for 'existing': ${tojson(validateResults)}`,
);

// 'late' and 'renamed' did not exist at 'atClusterTime' and should be skipped, so they produce a
// skip log (id 11790100) instead of a validation result.
const skipLogs = rawMongoProgramOutput("(11790100)");
assert(skipLogs.includes(`${dbName}.late`), `'late' should be skipped: ${skipLogs}`);
assert(skipLogs.includes(`${dbName}.renamed`), `'renamed' should be skipped: ${skipLogs}`);
assert(
    !resultsByNs[`${dbName}.late`],
    `'late' should not be validated: ${tojson(validateResults)}`,
);
assert(
    !resultsByNs[`${dbName}.renamed`],
    `'renamed' should not be validated: ${tojson(validateResults)}`,
);

const exitCode = runMongoProgram(
    "mongod",
    "--port",
    allocatePort(),
    "--dbpath",
    dbpath,
    "--validate",
    "--setParameter",
    `collectionValidateOptions=${tojson({options: {atClusterTime}})}`,
    "--setParameter",
    `validateDbName=${dbName}`,
    "--setParameter",
    `validateCollectionName=late`,
);
// we expect validate to fail for this single collection.
assert.neq(0, exitCode, "modal validate for missing collection at atClusterTime should fail");
const failLogs = rawMongoProgramOutput("(11790202)");
assert(failLogs.includes(`${dbName}.late`), `'late' should show validation failure: ${failLogs}`);
