/* Tests startup warning for deprecated authSchemaVersion
 *
 * This test requires auth schema version to persist across a restart.
 * @tags: [requires_persistence]
 */
(function() {
    "use strict";

    const dbpath = MongoRunner.dataPath + "mongodb_cr_deprecation_warning";
    resetDbpath(dbpath);

    {
        // Setup database using schema version 3
        const conn = MongoRunner.runMongod({dbpath: dbpath});
        conn.getDB("admin").system.version.update(
            {_id: "authSchema"}, {"currentVersion": 3}, {upsert: true});
        MongoRunner.stopMongod(conn);
    }

    {
        // Mount "old" database using new mongod
        const conn = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true, useLogFiles: true});
        const admin = conn.getDB("admin");

        // Look for our new warning
        assert.soon(function() {
            const log = cat(conn.fullOptions.logFile);
            return /WARNING: This server is using MONGODB-CR/.test(log);
        }, "No warning issued for MONGODB-CR usage", 30 * 1000, 5 * 1000);

        MongoRunner.stopMongod(conn);
    }
})();
