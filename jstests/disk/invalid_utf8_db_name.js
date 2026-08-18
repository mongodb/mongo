/**
 * Tests that a durable catalog entry whose database name is not valid UTF-8 is reported clearly
 * instead of failing with a confusing error on startup.
 *
 * The corruption is applied offline with the WiredTiger tool: the shell re-encodes JS strings as
 * well-formed UTF-8, so an invalid name cannot be created through any client operation.
 *
 * @tags: [requires_wiredtiger]
 */

import {
    rewriteCatalogTableHex,
    startMongodOnExistingPath,
} from "jstests/disk/libs/wt_file_helper.js";
import {before, describe, it} from "jstests/libs/mochalite.js";

// The trailing 'p' (0x70) of the database name is flipped to 0x80, which cannot begin a UTF-8
// sequence. That makes the name invalid at a known offset while leaving its length unchanged, so
// the surrounding catalog BSON stays well-formed.
const dbName = "invalidUtf8Dbp";
const collName = "coll";
const dbpath = MongoRunner.dataPath + "invalid_utf8_db_name/";

const logIdRegex = (id) => new RegExp(`"id"\\s*:\\s*${id}\\s*,`);

const toHex = (str) =>
    str
        .split("")
        .map((c) => c.charCodeAt(0).toString(16).padStart(2, "0"))
        .join("");

// Creates a dbpath holding a database whose catalog entry names it with invalid UTF-8. The modes
// under test write to the dbpath, so each one gets its own freshly corrupted copy.
function makeCorruptDbpath(path) {
    resetDbpath(path);

    let conn = startMongodOnExistingPath(path);
    assert.commandWorked(conn.getDB(dbName)[collName].insert({a: 1}));
    // A second database with a valid name, to confirm the failure names the offending one.
    assert.commandWorked(conn.getDB("validDb")[collName].insert({a: 1}));

    // The catalog can only be edited while mongod is not running.
    MongoRunner.stopMongod(conn, null, {skipValidation: true});

    const validHex = toHex(dbName);
    const invalidHex = validHex.slice(0, -2) + "80";
    let replacements = 0;
    rewriteCatalogTableHex(conn, (line) => {
        if (!line.includes(validHex)) {
            return line;
        }
        ++replacements;
        return line.replaceAll(validHex, invalidHex);
    });
    assert.gt(replacements, 0, "found no catalog entry containing the database name", {
        dbName,
        validHex,
    });
}

describe("a database name that is not valid UTF-8 in the durable catalog", function () {
    before(function () {
        makeCorruptDbpath(dbpath);
    });

    it("makes startup fail, naming the database and explaining the corruption", function () {
        clearRawMongoProgramOutput();
        let conn = MongoRunner.runMongod({
            dbpath: dbpath,
            noCleanData: true,
            waitForConnect: false,
        });

        assert.soon(
            () => logIdRegex(20557).test(rawMongoProgramOutput('"id":20557')),
            "mongod did not fail to start up with a corrupt database name",
        );

        const output = rawMongoProgramOutput(".*");

        // Check the logs for the various errors that point to the bad db name.
        assert(logIdRegex(13340100).test(output), "missing the startup diagnostic");
        assert.gte(
            output.search(/durable catalog is likely corrupt/),
            0,
            "startup diagnostic did not explain the corruption",
        );
        assert(logIdRegex(11379210).test(output), "missing the case-folding diagnostic");
        assert.gte(
            output.search(new RegExp(toHex(dbName).slice(0, -2) + "80", "i")),
            0,
            "case-folding diagnostic did not report the raw bytes of the name",
        );

        assert.gte(output.search(/Non UTF-8 data encountered/), 0, "missing the ICU error");

        MongoRunner.stopMongod(conn, null, {
            allowedExitCode: MongoRunner.EXIT_UNCAUGHT,
            skipValidation: true,
        });
    });

    it("makes --repair fail", function () {
        const modeDbpath = MongoRunner.dataPath + "invalid_utf8_db_name_repair/";
        makeCorruptDbpath(modeDbpath);

        clearRawMongoProgramOutput();
        assert.neq(
            0,
            runMongoProgram("mongod", "--repair", "--port", allocatePort(), "--dbpath", modeDbpath),
            "mongod --repair unexpectedly succeeded on a corrupt database name",
        );

        // --repair reaches a database through repair::repairDatabase, which closes and reopens it
        // directly rather than going through the startup helper, so only the case-folding
        // diagnostic is logged.
        assert(
            logIdRegex(11379210).test(rawMongoProgramOutput(".*")),
            "missing the case-folding diagnostic under --repair",
        );
    });

    // Validation is a diagnostic mode, so it reports the database it cannot open and validates the
    // rest instead of ending startup at the first corrupt name.
    const validateModes = {
        "--validate": {args: ["--validate"], expectSummary: true},
        "--validateParallel": {args: ["--validateParallel", "2"], expectSummary: false},
    };

    for (const [mode, {args, expectSummary}] of Object.entries(validateModes)) {
        it(`makes ${mode} skip the database and validate the rest`, function () {
            const modeDbpath = MongoRunner.dataPath + "invalid_utf8_db_name" + mode + "/";
            makeCorruptDbpath(modeDbpath);

            clearRawMongoProgramOutput();

            // Validation could not complete for every database, so the exit code is still a
            // failure.
            assert.neq(
                0,
                runMongoProgram(
                    "mongod",
                    "--port",
                    allocatePort(),
                    "--dbpath",
                    modeDbpath,
                    ...args,
                ),
                `mongod ${mode} unexpectedly succeeded on a corrupt database name`,
            );

            const output = rawMongoProgramOutput(".*");

            // The database that could not be opened is reported and skipped, not fatal.
            assert(
                logIdRegex(11379210).test(output),
                `missing the case-folding diagnostic under ${mode}`,
            );
            assert(
                logIdRegex(13340100).test(output),
                `missing the startup diagnostic under ${mode}`,
            );
            assert(
                logIdRegex(13340101).test(output),
                `${mode} did not report skipping the database`,
            );
            assert(
                !logIdRegex(20557).test(output),
                `${mode} should not terminate startup with an uncaught exception`,
            );

            // Every other database was still validated, and validation reported its own completion
            // rather than being cut short.
            assert.gte(
                output.search(/"ns":"validDb\.coll"/),
                0,
                `${mode} did not validate the database with a valid name`,
            );
            if (expectSummary) {
                assert(
                    logIdRegex(9437304).test(output),
                    `${mode} did not report that validation found issues`,
                );
            }
            assert(logIdRegex(9437300).test(output), `${mode} did not report validation failure`);
        });
    }
});
