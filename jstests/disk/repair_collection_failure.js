/**
 * Checks that when a collection's index rebuild throws an exception during a database repair, there
 * exists a log message indicative of where the exception occurred. SERVER-42014 contains additional
 * details.
 *
 *  @tags: [requires_wiredtiger]
 */

(function() {
    /**
     * Returns information to enable exitBeforeIndexRepair and test it causes collection collA to
     * fail upon index rebuild during a database repair.
     */
    const constructFailpointInfo = function constructExitBeforeIndexRepairFailpointInfo(collA) {
        const failpoint = 'exitBeforeIndexRepair';

        // Specific failpoint activation parameters.
        const namespace = collA.getFullName();
        const mode = 'alwaysOn';
        const data = {namespace};

        // The message expected to be logged upon repair database failure.
        const msg = `Failpoint ${failpoint} has been enabled. Failure while building` +
            ` indexes on ${namespace}`;

        return {failpoint, msg, mode, data};
    };

    /**
     * Returns a running mongod process.
     */
    const setupRepairDB = function setupRepairDatabaseForFailpoint(dbName, dbpath) {
        resetDbpath(dbpath);
        return MongoRunner.runMongod({dbpath: dbpath, setParameter: {enableTestCommands: 1}});
    };

    /**
     * Sets up a database with two indexed collections. Returns the main collection that will cause
     * repair to fail.
     */
    const setupData = function(mongod, dbName) {
        const collA = mongod.getDB(dbName)["collA"];
        const collB = mongod.getDB(dbName)["collB"];

        // Ensures there are two collections that have indexes on them. The failpoint should only
        // throw an exception during the index rebuild for collection A.
        assert.commandWorked(collA.createIndex({"fieldA": 1}));
        assert.commandWorked(collB.createIndex({"fieldB": -1}));
        return collA;
    };

    /**
     * Checks the behavior when a database repair is activated upon starting a mongod instance with
     * command line argument --repair and failpoint exitBeforeIndexRepair is enabled. Expects the
     * failpoint to cause a failure during the repair while rebuilding the indexes on a specified
     * collection, collection A, and for the error message to be logged accordingly. The server
     * should crash.
     */
    const testRepairCmdLine = function testRepairThroughCmdLine(dbName, dbpath) {
        const mongod = setupRepairDB(dbName, dbpath);
        const collA = setupData(mongod, dbName);

        clearRawMongoProgramOutput();

        // Get info about the failpoint that causes collection A's index rebuild to fail during a
        // database repair.
        const {failpoint, msg, mode, data} = constructFailpointInfo(collA);
        const param = `failpoint.${failpoint}=${JSON.stringify({mode, data})})}`;
        const port = mongod.port;

        MongoRunner.stopMongod(mongod);

        // Prompt a database repair upon starting mongod that fails during collection A's index
        // rebuild.
        assert.eq(
            MongoRunner.EXIT_ABRUPT,
            runMongoProgram(
                "mongod", "--repair", "--port", port, "--dbpath", dbpath, "--setParameter", param));

        // Check that the failure during collection A's index rebuild was logged.
        assert.neq(
            -1, rawMongoProgramOutput().indexOf(msg), "Could not find " + msg + " in log output");
    };

    /**
     * Checks the behavior when a database repair is activated using the shell command
     * db.repairDatabase() and failpoint exitBeforeIndexRepair is enabled. Expects the failpoint to
     * cause a failure during the repair while rebuilding the indexes on a specified collection,
     * collection A, and for the error message to be logged accordingly. The server should not
     * crash.
     */
    const testRepairShell = function testRepairThroughShell(dbName, dbpath) {
        const mongod = setupRepairDB(dbName, dbpath);
        const collA = setupData(mongod, dbName);

        clearRawMongoProgramOutput();

        // Get info about the failpoint that causes collection A's index rebuild to fail during a
        // database repair.
        const {failpoint, msg, mode, data} = constructFailpointInfo(collA);

        assert.commandWorked(
            collA.getDB().adminCommand({configureFailPoint: failpoint, mode, data}));

        // A database repair should the command to fail on collection A's index rebuild.
        assert.commandFailed(collA.getDB().repairDatabase());

        // Check that the failure during collection A's index rebuild was logged.
        assert.neq(
            -1, rawMongoProgramOutput().indexOf(msg), "Could not find " + msg + " in log output");
        MongoRunner.stopMongod(mongod);
    };

    const dbName = "repair_collection_failure";
    const dbpath = `${MongoRunner.dataPath}${dbName}/`;

    // Test behavior when a database repair is activated upon starting a mongod instance with
    // --repair.
    testRepairCmdLine(dbName, dbpath);

    // Test behavior when a database repair is activated through shell command db.repairDatabase().
    testRepairShell(dbName, dbpath);

})();
