/**
 * Tests that the '--validate' takes in a specific database / collection and validates it modally.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 * ]
 */

import {parseValidateOutputsFromLogs} from "jstests/noPassthrough/validate/libs/validate_find_repl_set_divergence.js";
import {it, before, beforeEach, describe} from "jstests/libs/mochalite.js";

function setupCollections(db) {
    assert.commandWorked(db.createCollection("ham"));
    assert.commandWorked(db["ham"].insert({a: 1}));
    assert.commandWorked(db["ham"].createIndex({b: 1}));

    assert.commandWorked(db.createCollection("cheese"));
    assert.commandWorked(db["cheese"].insert({a: 1}));
    assert.commandWorked(db["cheese"].createIndex({b: 1}));
}

// The "validating collection" log line reports the options validation actually resolved to, which
// may differ from those the caller specified.
function parseValidationOptionsFromLogs() {
    return rawMongoProgramOutput('"id":20303')
        .split("\n")
        .filter((line) => line.trim() !== "")
        .map((line) => JSON.parse(line.split("|").slice(1).join("|")));
}

function generateResults(dbpath, opts) {
    MongoRunner.runMongod({
        dbpath: dbpath,
        validate: "",
        setParameter: opts,
        noCleanData: true,
    });
    return parseValidateOutputsFromLogs();
}

// TODO SERVER-76346 reconfigure modes when feature flag is not used to switch.
const kValidateModes = [
    {
        name: "serial validation",
        params: {featureFlagParallelCollectionValidation: false},
    },
    {
        name: "parallel validation using all cores",
        params: {featureFlagParallelCollectionValidation: true},
    },
    {
        name: "parallel validation limited to 8 namespaces",
        params: {
            featureFlagParallelCollectionValidation: true,
            validateParallelMaxConcurrentNamespaces: 8,
        },
    },
];

const dbpath = MongoRunner.dataPath + "modal_validate_specify";
let port;

describe("Modal Validate can specify target Databases and Collections", () => {
    // Setup the dbpath for this test.

    before(() => {
        // Setup DBs and collections
        const conn = MongoRunner.runMongod({dbpath: dbpath});
        port = conn.port;
        const testDB = conn.getDB("test");
        const test2DB = conn.getDB("test2");
        const adminDB = conn.getDB("admin");
        setupCollections(testDB);
        setupCollections(test2DB);
        setupCollections(adminDB);

        MongoRunner.stopMongod(conn);
        clearRawMongoProgramOutput();
    });

    beforeEach(() => clearRawMongoProgramOutput());

    for (const validateMode of kValidateModes) {
        it(`Command validates every namespace with ${validateMode.name}`, () => {
            const validateLogs = generateResults(dbpath, validateMode.params);
            jsTest.log.info("Validate logs", {validateLogs});
            const validatedNss = new Set(validateLogs.map((log) => log.attr.results.ns));
            for (const ns of [
                "test.ham",
                "test.cheese",
                "admin.ham",
                "admin.cheese",
                "test2.ham",
                "test2.cheese",
            ]) {
                assert(validatedNss.has(ns), `Expected ${ns} to be validated`, {validatedNss});
            }
        });

        it(`Command validates everything in the specified DB with ${validateMode.name}`, () => {
            const validateLogs = generateResults(dbpath, {
                ...validateMode.params,
                validateDbName: "test",
            });
            jsTest.log.info(validateLogs);
            assert.eq(2, validateLogs.length);
            const firstResult = validateLogs[0].attr.results;
            const secondResult = validateLogs[1].attr.results;
            assert(firstResult.ns == "test.ham" || firstResult.ns == "test.cheese");
            assert(secondResult.ns == "test.ham" || secondResult.ns == "test.cheese");
            assert.eq(2, firstResult.nIndexes);
            assert.eq(2, secondResult.nIndexes);
        });

        it(`Reports successful log for ${validateMode.name}`, () => {
            generateResults(dbpath, validateMode.params);
            const successLogs = rawMongoProgramOutput("9437303")
                .split("\n")
                .filter((line) => line.trim() !== "");
            assert.eq(1, successLogs.length, "Expected exactly one success log", {successLogs});
        });

        it(`Reports unsuccessful log for ${validateMode.name}`, () => {
            generateResults(dbpath, {
                ...validateMode.params,
                "failpoint.failRecordStoreTraversal": tojson({mode: "alwaysOn"}),
            });
            const failureLogs = rawMongoProgramOutput("9437304")
                .split("\n")
                .filter((line) => line.trim() !== "");
            assert.eq(1, failureLogs.length, "Expected exactly one failure log", {failureLogs});
            assert.eq(0, rawMongoProgramOutput("9437303").trim().length, "Unexpected success log");
        });
    }

    it("Command validates everything in the specified collection", () => {
        const validateLogs = generateResults(dbpath, {
            validateDbName: "test",
            validateCollectionName: "ham",
        });
        jsTest.log.info(validateLogs);
        assert.eq(1, validateLogs.length);
        const validateResult = validateLogs[0].attr.results;
        assert.eq("test.ham", validateResult.ns);
        assert.eq(2, validateResult.nIndexes);
    });

    it("Errors if collection is non-existent", () => {
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
                "validateDbName=test",
                "--setParameter",
                "validateCollectionName=lettuce",
            ),
        );
    });

    it("Can be run with repair:false when running modal validation.", () => {
        assert.eq(
            MongoRunner.EXIT_CLEAN,
            runMongoProgram(
                "mongod",
                "--validate",
                "--port",
                port,
                "--dbpath",
                dbpath,
                "--setParameter",
                "validateDbName=test",
                "--setParameter",
                "validateCollectionName=ham",
                "--setParameter",
                `collectionValidateOptions={options: {repair: false}}`,
            ),
        );
        const optionLogs = parseValidationOptionsFromLogs();
        assert.eq(1, optionLogs.length, {optionLogs});
        assert.eq("test.ham", optionLogs[0].attr.namespace);
        assert.eq("foreground", optionLogs[0].attr.options.mode);
        assert.eq(false, optionLogs[0].attr.options.repair);
    });

    it("Cannot be run with repair:true when running modal validation.", () => {
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
                "validateDbName=test",
                "--setParameter",
                "validateCollectionName=ham",
                "--setParameter",
                `collectionValidateOptions={options: {repair: true}}`,
            ),
        );
    });

    it("Can be run with fixMultikey:false when running modal validation.", () => {
        assert.eq(
            MongoRunner.EXIT_CLEAN,
            runMongoProgram(
                "mongod",
                "--validate",
                "--port",
                port,
                "--dbpath",
                dbpath,
                "--setParameter",
                "validateDbName=test",
                "--setParameter",
                "validateCollectionName=ham",
                "--setParameter",
                `collectionValidateOptions={options: {fixMultikey: false}}`,
            ),
        );
        const optionLogs = parseValidationOptionsFromLogs();
        assert.eq(1, optionLogs.length, {optionLogs});
        assert.eq("test.ham", optionLogs[0].attr.namespace);
        assert.eq("foreground", optionLogs[0].attr.options.mode);
        assert.eq(false, optionLogs[0].attr.options.fixMultikey);
    });

    it("Cannot be run with fixMultikey:true when running modal validation.", () => {
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
                "validateDbName=test",
                "--setParameter",
                "validateCollectionName=ham",
                "--setParameter",
                `collectionValidateOptions={options: {fixMultikey: true}}`,
            ),
        );
    });
});
