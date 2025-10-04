/**
 * This test attempts to read from the "local.startup_log" collection and assert that it has an
 * entry matching the server's response from the "getCmdLineOpts" command. The former operation may
 * be routed to a secondary in the replica set, whereas the latter must be routed to the primary.
 *
 * @tags: [
 *  # The test runs commands that are not allowed with security token: getCmdLineOpts.
 *  not_allowed_with_signed_security_token,
 *  assumes_read_preference_unchanged,
 *  requires_collstats,
 *  requires_capped,
 * ]
 */

import {arrayEq} from "jstests/aggregation/extras/utils.js";

// Check that smallArray is entirely contained by largeArray
// returns false if a member of smallArray is not in largeArray
function arrayIsSubset(smallArray, largeArray) {
    for (let i = 0; i < smallArray.length; i++) {
        if (!Array.contains(largeArray, smallArray[i])) {
            print("Could not find " + smallArray[i] + " in largeArray");
            return false;
        }
    }

    return true;
}

// Test startup_log
let stats = db.getSiblingDB("local").startup_log.stats();
assert(stats.capped);

let latestStartUpLog = db.getSiblingDB("local").startup_log.find().sort({$natural: -1}).limit(1).next();
let serverStatus = db._adminCommand("serverStatus");
let cmdLine = db._adminCommand("getCmdLineOpts").parsed;

// Test that the startup log has the expected keys
let verbose = false;
var expectedKeys = ["_id", "hostname", "startTime", "startTimeLocal", "cmdLine", "pid", "buildinfo"];
var keys = Object.keySet(latestStartUpLog);
assert(arrayEq(expectedKeys, keys, verbose), "startup_log keys failed");

// Tests _id implicitly - should be comprised of host-timestamp
// Setup expected startTime and startTimeLocal from the supplied timestamp
let _id = latestStartUpLog._id.split("-"); // _id should consist of host-timestamp
let _idUptime = _id.pop();
let _idHost = _id.join("-");
let uptimeSinceEpochRounded = Math.floor(_idUptime / 1000) * 1000;
let startTime = new Date(uptimeSinceEpochRounded); // Expected startTime

assert.eq(_idHost, latestStartUpLog.hostname, "Hostname doesn't match one from _id");
assert.eq(serverStatus.host.split(":")[0], latestStartUpLog.hostname, "Hostname doesn't match one in server status");
assert.closeWithinMS(startTime, latestStartUpLog.startTime, "StartTime doesn't match one from _id", 2000); // Expect less than 2 sec delta
assert.eq(cmdLine, latestStartUpLog.cmdLine, "cmdLine doesn't match that from getCmdLineOpts");
assert.eq(serverStatus.pid, latestStartUpLog.pid, "pid doesn't match that from serverStatus");

// Test buildinfo
let buildinfo = db.runCommand("buildinfo");
delete buildinfo.ok; // Delete extra meta info not in startup_log
delete buildinfo.operationTime; // Delete extra meta info not in startup_log
delete buildinfo.$clusterTime; // Delete extra meta info not in startup_log
delete buildinfo.lastCommittedOpTime; // Delete extra meta info not in startup_log (only returned
// by shardsvrs)
let hello = db._adminCommand("hello");

// Test buildinfo has the expected keys
var expectedKeys = [
    "version",
    "gitVersion",
    "allocator",
    "versionArray",
    "javascriptEngine",
    "openssl",
    "buildEnvironment",
    "debug",
    "maxBsonObjectSize",
    "bits",
    "modules",
];

var keys = Object.keySet(latestStartUpLog.buildinfo);
// Disabled to check
assert(
    arrayIsSubset(expectedKeys, keys),
    "buildinfo keys failed! \n expected:\t" + expectedKeys + "\n actual:\t" + keys,
);
assert.eq(buildinfo, latestStartUpLog.buildinfo, "buildinfo doesn't match that from buildinfo command");

// Test version and version Array
let version = latestStartUpLog.buildinfo.version.split("-")[0];
let versionArray = latestStartUpLog.buildinfo.versionArray;
let versionArrayCleaned = versionArray.slice(0, 3);
if (versionArray[3] == -100) {
    versionArrayCleaned[2] -= 1;
}

assert.eq(
    serverStatus.version,
    latestStartUpLog.buildinfo.version,
    "Mongo version doesn't match that from ServerStatus",
);
assert.eq(version, versionArrayCleaned.join("."), "version doesn't match that from the versionArray");
let jsEngine = latestStartUpLog.buildinfo.javascriptEngine;
assert(jsEngine == "none" || jsEngine.startsWith("mozjs"));
assert.eq(
    hello.maxBsonObjectSize,
    latestStartUpLog.buildinfo.maxBsonObjectSize,
    "maxBsonObjectSize doesn't match one from hello",
);
