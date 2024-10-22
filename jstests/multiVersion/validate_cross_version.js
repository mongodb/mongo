/**
 * Tests that the '--validate' command-line flag works on historic mongo collections.
 * Checks that validate works when there are no errors and correctly reports when
 * there are validation errors
 */

import "jstests/multiVersion/libs/verify_versions.js";

import {getUriForIndex, runWiredTigerTool} from "jstests/disk/libs/wt_file_helper.js";
import {allLtsVersions} from "jstests/multiVersion/libs/lts_versions.js";

// Setup the dbpath for this test.
const dbpath = MongoRunner.dataPath + 'validate_cross_version';

function corruptIndex(conn) {
    let coll = conn.getDB("test").getCollection("collect");
    const uri = getUriForIndex(coll, "_id_");
    MongoRunner.stopMongod(conn, null, {skipValidation: true});
    runWiredTigerTool("-h", conn.dbpath, "truncate", uri);
}

function testVersion(binVersion, fcv, shouldCorrupt) {
    jsTestLog("Testing Version: " + binVersion + ", corrupted index? " + shouldCorrupt);
    resetDbpath(dbpath);
    let opts = {dbpath: dbpath, binVersion: binVersion};
    let conn = MongoRunner.runMongod(opts);

    let adminDB = conn.getDB("admin");
    const res = adminDB.runCommand({"setFeatureCompatibilityVersion": fcv});
    if (!res.ok && res.code === 7369100) {
        // We failed due to requiring 'confirm: true' on the command. This will only
        // occur on 7.0+ nodes that have 'enableTestCommands' set to false. Retry the
        // setFCV command with 'confirm: true'.
        assert.commandWorked(adminDB.runCommand({
            "setFeatureCompatibilityVersion": fcv,
            confirm: true,
        }));
    } else {
        assert.commandWorked(res, "Failed to run command with args: " + binVersion + " " + fcv);
    }

    let testDB1 = conn.getDB('test');
    const port = conn.port;
    assert.commandWorked(testDB1.createCollection('collect'));
    assert.commandWorked(testDB1['collect'].insert({a: 1}));
    assert.commandWorked(testDB1['collect'].createIndex({b: 1}));

    if (shouldCorrupt) {
        corruptIndex(conn);
    } else {
        MongoRunner.stopMongod(conn, null, {skipValidation: true});
    }
    clearRawMongoProgramOutput();

    jsTestLog("Beginning command line validation");

    MongoRunner.runMongod({port: port, dbpath: dbpath, validate: "", noCleanData: true});

    let validateLogs = rawMongoProgramOutput("(9437301|9437303|9437304)")
                           .split("\n")
                           .filter(line => line.trim() !== "")
                           .map(line => JSON.parse(line.split("|").slice(1).join("|")));

    let results = validateLogs.filter(
        json => (json.id === 9437301 && json.attr.results.ns == "test.collect"));

    assert.eq(1, results.length);
    let result = results[0].attr.results;
    assert(result, "Couldn't find validation result for test.collect");
    jsTestLog(result);

    assert.eq(false, result.repaired);
    assert.eq(true, result.indexDetails.b_1.valid);
    assert.eq(!shouldCorrupt, result.indexDetails["_id_"].valid);
    assert.eq(1, result.nrecords);
    assert.eq(2, result.nIndexes);
    assert.eq(1, result.keysPerIndex["b_1"]);

    if (shouldCorrupt) {
        assert.eq(1, validateLogs.filter(json => json.id === 9437304).length);
    } else {
        assert.eq(1, validateLogs.filter(json => json.id === 9437303).length);
    }

    return result;
}

for (let i = 0; i < allLtsVersions.length; i++) {
    testVersion(allLtsVersions[i].binVersion, allLtsVersions[i].featureCompatibilityVersion, false);
}

for (let i = 0; i < allLtsVersions.length; i++) {
    testVersion(allLtsVersions[i].binVersion, allLtsVersions[i].featureCompatibilityVersion, true);
}
