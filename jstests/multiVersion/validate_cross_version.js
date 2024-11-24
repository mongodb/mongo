/**
 * Tests that the '--validate' command-line flag works on historic mongo collections.
 * Checks that validate works when there are no errors and correctly reports when
 * there are validation errors
 */

import "jstests/multiVersion/libs/verify_versions.js";

import {
    getUriForColl,
    getUriForIndex,
    runWiredTigerTool
} from "jstests/disk/libs/wt_file_helper.js";
import {allLtsVersions} from "jstests/multiVersion/libs/lts_versions.js";

// Setup the dbpath for this test.
const dbpath = MongoRunner.dataPath + 'validate_cross_version';

function setupCommon(db) {
    assert.commandWorked(db.createCollection('collect'));
    assert.commandWorked(db['collect'].insert({a: 1}));
    assert.commandWorked(db['collect'].createIndex({b: 1}));
}

function checkCommon(validateLogs, shouldCorrupt) {
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
}

function setup4(db) {
    // Removed in 5.0 as per https://www.mongodb.com/docs/v6.2/core/geohaystack/
    assert.commandWorked(db.createCollection('geohaystack'));
    assert.commandWorked(
        db['geohaystack'].insert({_id: 100, pos: {lng: 126.9, lat: 35.2}, type: "restaurant"}));
    assert.commandWorked(
        db['geohaystack'].insert({_id: 200, pos: {lng: 127.5, lat: 36.1}, type: "restaurant"}));
    assert.commandWorked(
        db['geohaystack'].createIndex({pos: "geoHaystack", type: 1}, {bucketSize: 1}));
}

function corrupt4(conn) {
    // Check _id_ because we can't even open pos_geoHaystack_type_1
    return [getUri(conn, "geohaystack", "_id_")];
}

function check4(validateLogs, shouldCorrupt) {
    let results = validateLogs.filter(
        json => (json.id === 9437301 && json.attr.results.ns == "test.geohaystack"));
    assert.eq(1, results.length);
    let result = results[0].attr.results;
    assert(result, "Couldn't find validation result for test.geohaystack");
    jsTestLog(result);

    assert.eq(!shouldCorrupt, result.valid);
    // The geohaystack index isn't visible
    assert.eq(1, result.nIndexes);
    assert.eq(!shouldCorrupt, result.indexDetails["_id_"].valid);
    assert.eq(2, result.nrecords);
}

function setup5(db) {
    // https://www.mongodb.com/docs/manual/core/timeseries-collections/#std-label-manual-timeseries-collection
    // May have changed in 6.0 as it wasn't downgrade safe
    assert.commandWorked(db.createCollection("weather", {
        timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "seconds"},
        expireAfterSeconds: 86400
    }));
    assert.commandWorked(db.weather.insertMany([
        {
            metadata: {sensorId: 5578, type: "temperature"},
            timestamp: ISODate("2021-05-18T00:00:00.000Z"),
            temp: 12,
        },
        {
            metadata: {sensorId: 5578, type: "temperature"},
            timestamp: ISODate("2021-05-18T04:00:00.000Z"),
            temp: 11,
        },
        {
            metadata: {sensorId: 5578, type: "temperature"},
            timestamp: ISODate("2021-05-18T08:00:00.000Z"),
        }
    ]));
    assert.commandWorked(db.weather.createIndex({"metadata.sensorId": 1}));
    // https://www.mongodb.com/docs/manual/release-notes/6.0-compatibility/#index-key-format
    assert.commandWorked(db.createCollection("uniqueColl"));
    assert.commandWorked(db.uniqueColl.insertMany(
        [{uniq: 14, value: 23}, {uniq: 11, value: 17}, {uniq: 21, value: 23}]));
    assert.commandWorked(db.uniqueColl.createIndex({uniq: 1}, {unique: true}));
}

function corrupt5(conn) {
    // Truncate the collection to force validation to traverse the indexes that potentially have
    // strange formats
    return [
        getUriForColl(conn.getDB("test").getCollection("weather")),
        getUriForColl(conn.getDB("test").getCollection("uniqueColl"))
    ];
}

function check5(validateLogs, shouldCorrupt) {
    let weatherResults = validateLogs.filter(
        json => (json.id === 9437301 && json.attr.results.ns == "test.system.buckets.weather"));
    assert.eq(1, weatherResults.length);
    let weatherResult = weatherResults[0].attr.results;
    assert(weatherResult, "Couldn't find validation result for test.weather");
    jsTestLog(weatherResult);

    assert.eq(!shouldCorrupt, weatherResult.valid);
    assert.eq(1, weatherResult.nIndexes);
    assert.eq(!shouldCorrupt, weatherResult.indexDetails["metadata.sensorId_1"].valid);
    if (shouldCorrupt) {
        assert.eq(3, weatherResult.extraIndexEntries.length);
    } else {
        assert.eq(3, weatherResult.nrecords);
    }

    let uniqResults = validateLogs.filter(
        json => (json.id === 9437301 && json.attr.results.ns == "test.uniqueColl"));
    assert.eq(1, uniqResults.length);
    let uniqResult = uniqResults[0].attr.results;
    assert(uniqResult, "Couldn't find validation result for test.uniqueColl");
    jsTestLog(uniqResult);

    assert.eq(!shouldCorrupt, uniqResult.valid);
    assert.eq(2, uniqResult.nIndexes);
    assert.eq(!shouldCorrupt, uniqResult.indexDetails["_id_"].valid);
    assert.eq(!shouldCorrupt, uniqResult.indexDetails["uniq_1"].valid);
    if (shouldCorrupt) {
        assert.eq(6, uniqResult.extraIndexEntries.length);
    } else {
        assert.eq(3, uniqResult.nrecords);
    }
}

const versionSpecificSetup = {
    "4.4": {setup: setup4, corrupt: corrupt4, validate: check4},
    "5.0": {setup: setup5, corrupt: corrupt5, validate: check5},
};

function getUri(conn, collection = "collect", indexName = "_id_") {
    let coll = conn.getDB("test").getCollection(collection);
    const uri = getUriForIndex(coll, indexName);
    return uri;
}

function corruptUris(dbpath, uris) {
    for (let i = 0; i < uris.length; i++) {
        runWiredTigerTool("-h", dbpath, "truncate", uris[i]);
    }
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
    setupCommon(testDB1);

    if (versionSpecificSetup[binVersion]) {
        versionSpecificSetup[binVersion].setup(testDB1);
    }

    if (shouldCorrupt) {
        let toTruncate = [];
        toTruncate.push(getUri(conn));
        if (versionSpecificSetup[binVersion]) {
            toTruncate = toTruncate.concat(versionSpecificSetup[binVersion].corrupt(conn));
        }
        jsTestLog(toTruncate);
        MongoRunner.stopMongod(conn, null, {skipValidation: true});
        corruptUris(conn.dbpath, toTruncate);
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

    checkCommon(validateLogs, shouldCorrupt);

    if (versionSpecificSetup[binVersion]) {
        versionSpecificSetup[binVersion].validate(validateLogs, shouldCorrupt);
    }
}

for (let i = 0; i < allLtsVersions.length; i++) {
    testVersion(allLtsVersions[i].binVersion, allLtsVersions[i].featureCompatibilityVersion, false);
}

for (let i = 0; i < allLtsVersions.length; i++) {
    testVersion(allLtsVersions[i].binVersion, allLtsVersions[i].featureCompatibilityVersion, true);
}
