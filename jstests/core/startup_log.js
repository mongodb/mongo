load('jstests/aggregation/extras/utils.js');

(function() {
    'use strict';

    // Check that smallArray is entirely contained by largeArray
    // returns false if a member of smallArray is not in largeArray
    function arrayIsSubset(smallArray, largeArray) {
        for (var i = 0; i < smallArray.length; i++) {
            if (!Array.contains(largeArray, smallArray[i])) {
                print("Could not find " + smallArray[i] + " in largeArray");
                return false;
            }
        }

        return true;
    }

    // Test startup_log
    var stats = db.getSisterDB("local").startup_log.stats();
    assert(stats.capped);

    var latestStartUpLog =
        db.getSisterDB("local").startup_log.find().sort({$natural: -1}).limit(1).next();
    var serverStatus = db._adminCommand("serverStatus");
    var cmdLine = db._adminCommand("getCmdLineOpts").parsed;

    // Test that the startup log has the expected keys
    var verbose = false;
    var expectedKeys =
        ["_id", "hostname", "startTime", "startTimeLocal", "cmdLine", "pid", "buildinfo"];
    var keys = Object.keySet(latestStartUpLog);
    assert(arrayEq(expectedKeys, keys, verbose), 'startup_log keys failed');

    // Tests _id implicitly - should be comprised of host-timestamp
    // Setup expected startTime and startTimeLocal from the supplied timestamp
    var _id = latestStartUpLog._id.split('-');  // _id should consist of host-timestamp
    var _idUptime = _id.pop();
    var _idHost = _id.join('-');
    var uptimeSinceEpochRounded = Math.floor(_idUptime / 1000) * 1000;
    var startTime = new Date(uptimeSinceEpochRounded);  // Expected startTime

    assert.eq(_idHost, latestStartUpLog.hostname, "Hostname doesn't match one from _id");
    assert.eq(serverStatus.host.split(':')[0],
              latestStartUpLog.hostname,
              "Hostname doesn't match one in server status");
    assert.closeWithinMS(startTime,
                         latestStartUpLog.startTime,
                         "StartTime doesn't match one from _id",
                         2000);  // Expect less than 2 sec delta
    assert.eq(cmdLine, latestStartUpLog.cmdLine, "cmdLine doesn't match that from getCmdLineOpts");
    assert.eq(serverStatus.pid, latestStartUpLog.pid, "pid doesn't match that from serverStatus");

    // Test buildinfo
    var buildinfo = db.runCommand("buildinfo");
    delete buildinfo.ok;  // Delete extra meta info not in startup_log
    var isMaster = db._adminCommand("ismaster");

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
        "modules"
    ];

    var keys = Object.keySet(latestStartUpLog.buildinfo);
    // Disabled to check
    assert(arrayIsSubset(expectedKeys, keys),
           "buildinfo keys failed! \n expected:\t" + expectedKeys + "\n actual:\t" + keys);
    assert.eq(buildinfo,
              latestStartUpLog.buildinfo,
              "buildinfo doesn't match that from buildinfo command");

    // Test version and version Array
    var version = latestStartUpLog.buildinfo.version.split('-')[0];
    var versionArray = latestStartUpLog.buildinfo.versionArray;
    var versionArrayCleaned = versionArray.slice(0, 3);
    if (versionArray[3] == -100) {
        versionArrayCleaned[2] -= 1;
    }

    assert.eq(serverStatus.version,
              latestStartUpLog.buildinfo.version,
              "Mongo version doesn't match that from ServerStatus");
    assert.eq(
        version, versionArrayCleaned.join('.'), "version doesn't match that from the versionArray");
    var jsEngine = latestStartUpLog.buildinfo.javascriptEngine;
    assert((jsEngine == "none") || jsEngine.startsWith("mozjs"));
    assert.eq(isMaster.maxBsonObjectSize,
              latestStartUpLog.buildinfo.maxBsonObjectSize,
              "maxBsonObjectSize doesn't match one from ismaster");

})();
