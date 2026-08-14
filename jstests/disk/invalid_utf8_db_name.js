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
            () => rawMongoProgramOutput("(20557|13340100)").search(/20557/) >= 0,
            "mongod did not fail to start up with a corrupt database name",
        );

        const output = rawMongoProgramOutput(".*");

        // Check the logs for the various errors that point to the bad db name.
        assert.gte(output.search(/13340100/), 0, "missing the startup diagnostic");
        assert.gte(
            output.search(/durable catalog is likely corrupt/),
            0,
            "startup diagnostic did not explain the corruption",
        );
        assert.gte(output.search(/11379210/), 0, "missing the case-folding diagnostic");
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

    // Each of these modes walks every database in the catalog, so all of them must explain the
    // corruption rather than failing opaquely. --repair reaches a database through
    // repair::repairDatabase, which closes and reopens it directly rather than going through the
    // startup helper, so it only logs the case-folding diagnostic.
    const modalStartups = {
        "--repair": {args: ["--repair"], expectStartupDiagnostic: false},
        "--validate": {args: ["--validate"], expectStartupDiagnostic: true},
        "--validateParallel": {args: ["--validateParallel", "2"], expectStartupDiagnostic: true},
    };

    for (const [mode, {args, expectStartupDiagnostic}] of Object.entries(modalStartups)) {
        it(`makes ${mode} fail the same way`, function () {
            const modeDbpath = MongoRunner.dataPath + "invalid_utf8_db_name" + mode + "/";
            makeCorruptDbpath(modeDbpath);

            clearRawMongoProgramOutput();
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
            assert.gte(
                output.search(/11379210/),
                0,
                `missing the case-folding diagnostic under ${mode}`,
            );
            if (expectStartupDiagnostic) {
                assert.gte(
                    output.search(/13340100/),
                    0,
                    `missing the startup diagnostic under ${mode}`,
                );
            }
        });
    }
});
