/**
 * Test Startup & Shutdown of an NT service.
 * Assumptions:
 * - Assumes mongodb and mongos service are not installed.
*/
if (_isWindows()) {
    (function() {
        'use strict';

        // Translate a relative path into a fully qualified path with a drive letter.
        // MongoRunner.dataPath produces paths without a drive letter.
        function GetFullPath(path) {
            var ret = run("cmd.exe", "/c", "dir " + path + " > " + path + "\\dir.log");
            assert(ret == 0, "cmd failed");

            var dirlog = cat(path + "\\dir.log");

            var matches = dirlog.match(/Directory of ([\w\\:]+)/);

            return matches[1];
        };

        function wait(seconds) {
            print("Sleeping for " + seconds + " seconds");
            sleep(seconds * 1000);
        };

        var name = "ntservice";
        var dbdir = MongoRunner.dataPath + name + "\\";
        dbdir = dbdir.replace(/\//g, "\\");

        print("DB Path: " + dbdir);
        // ensure db directory exists
        assert(mkdir(dbdir));

        dbdir = GetFullPath(dbdir);

        var logpath = dbdir;
        print("Log Path: " + logpath);

        function TestMongoD() {
            var ret = 0;

            // Verify it is not installed
            // 1060 = ERROR_SERVICE_DOES_NOT_EXIST, seew winerror.h
            ret = run("sc.exe", "query", "mongodb");
            assert(ret == 1060,
                   "Test expects no mongodb service to be installed 1, return code = " + ret);

            // Install service
            //
            ret = run("mongod.exe",
                      "--install",
                      "--dbpath",
                      dbdir,
                      "--logpath",
                      logpath + "\\ntservice.log");
            assert(ret == 0, "service install failed, return code = " + ret);

            // Give mongod time to release its file locks
            wait(5);

            print(cat(logpath + "\\ntservice.log"));

            ret = run("sc.exe", "query", "mongodb");
            assert(ret == 0, "service query failed 1, return code = " + ret);

            ret = run("sc.exe", "start", "mongodb");
            assert(ret == 0, "mongod start failed, return code = " + ret);

            // Let MongoDB run for a little bit
            wait(5);

            // We had a known bug where stop would not signal the SCM correctly
            ret = run("sc.exe", "stop", "mongodb");
            assert(ret == 0, "service stop failed, return code = " + ret);

            // Give mongod time to shutdown
            wait(5);

            // Reinstall service
            //
            ret = run("mongod.exe",
                      "--reinstall",
                      "--dbpath",
                      dbdir,
                      "--logpath",
                      logpath + "\\ntservice2.log");
            assert(ret == 0, "mongod reinstall failed, return code = " + ret);

            // Query to make sure it is still installed
            ret = run("sc.exe", "query", "mongodb");
            assert(ret == 0, "service query failed 2, return code = " + ret);

            // Remove service
            //
            ret = run("mongod.exe", "--remove");
            assert(ret == 0, "service remove failed, return code = " + ret);

            // Verify it is not installed
            // 1060 = ERROR_SERVICE_DOES_NOT_EXIST, seew winerror.h
            ret = run("sc.exe", "query", "mongodb");
            assert(ret == 1060,
                   "Test expects no mongodb service to be installed 2, return code = " + ret);
        };

        TestMongoD();

        function TestMongoS() {
            var ret = 0;

            // Verify it is not installed
            // 1060 = ERROR_SERVICE_DOES_NOT_EXIST, seew winerror.h
            ret = run("sc.exe", "query", "mongos");
            assert(ret == 1060, "Test expects no mongos service to be installed 1");

            // Install service with a bad config server, and make sure the service stops
            //
            ret = run("mongos.exe",
                      "--install",
                      "--logpath",
                      logpath + "\\ntservice3.log",
                      "--configdb",
                      "fake/localhost:20,fake/localhost:21,fake/localhost:22");
            assert(ret == 0, "service install failed, return code = " + ret);

            ret = run("sc.exe", "query", "mongos");
            assert(ret == 0, "service query failed 1, return code = " + ret);

            // Make sure mongos fails to start
            // 1053 = ERROR_SERVICE_REQUEST_TIMEOUT, see winerror.h
            ret = run("sc.exe", "start", "mongos");
            assert(ret == 1053, "mongos start failed, return code = " + ret);

            // Reinstall service
            //
            ret = run("mongos.exe",
                      "--reinstall",
                      "--logpath",
                      logpath + "\\ntservice4.log",
                      "--configdb",
                      "fake/localhost:20,fake/localhost:21,fake/localhost:22");
            assert(ret == 0, "mongos reinstall failed, return code = " + ret);

            ret = run("sc.exe", "query", "mongos");
            assert(ret == 0, "service query failed 2, return code = " + ret);

            // Remove service
            //
            ret = run("mongos.exe",
                      "--remove",
                      "--configdb",
                      "fake/localhost:20,fake/localhost:21,fake/localhost:22");
            assert(ret == 0, "service remove failed, return code = " + ret);

            // Verify it is not installed
            // 1060 = ERROR_SERVICE_DOES_NOT_EXIST, seew winerror.h
            ret = run("sc.exe", "query", "mongos");
            assert(ret == 1060, "Test expects no mongos service to be installed 2");
        };

        TestMongoS();

    })();

}  // if (is_Windows())
