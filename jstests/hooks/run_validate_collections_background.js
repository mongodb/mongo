/**
 * Runs the validate command with {background:true} against all nodes (replica set members and
 * standalone nodes, not sharded clusters) concurrently with running tests.
 */
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";
import {Thread} from "jstests/libs/parallelTester.js";

if (typeof db === 'undefined') {
    throw new Error(
        "Expected mongo shell to be connected a server, but global 'db' object isn't defined");
}

// Disable implicit sessions so FSM workloads that kill random sessions won't interrupt the
// operations in this test that aren't resilient to interruptions.
TestData.disableImplicitSessions = true;

const conn = db.getMongo();
const topology = DiscoverTopology.findConnectedNodes(conn);

/**
 * Returns true if the error code is transient and does not indicate data corruption.
 */
const isIgnorableError = function ignorableError(codeName) {
    if (codeName == "NamespaceNotFound" || codeName == "Interrupted" ||
        codeName == "CommandNotSupportedOnView" || codeName == "InterruptedAtShutdown" ||
        codeName == "InvalidViewDefinition" || codeName == "CommandNotSupported") {
        return true;
    }
    return false;
};

/**
 * Runs validate commands with {background:true} against 'host' for all collections it possesses.
 *
 * Returns the cumulative command failure results, if there are any, in an object
 * { ok: 0, error: [{cmd-res}, {cmd-res}, ... ]}
 * Or simply OK if all cmds were successful.
 * {ok: 1}
 *
 * This function should not throw if everything is working properly.
 */
const validateCollectionsBackgroundThread = function validateCollectionsBackground(
    host, isIgnorableErrorFunc) {
    // Calls 'func' with the print() function overridden to be a no-op.
    const quietly = (func) => {
        const printOriginal = print;
        try {
            print = Function.prototype;
            func();
        } finally {
            print = printOriginal;
        }
    };

    // Suppress the log messages generated establishing new mongo connections. The
    // run_validate_collections_background.js hook is executed frequently by resmoke.py and
    // could lead to generating an overwhelming amount of log messages.
    let conn;
    quietly(() => {
        conn = new Mongo(host);
    });
    assert.neq(null,
               conn,
               "Failed to connect to host '" + host + "' for background collection validation");

    // Filter out arbiters.
    if (conn.adminCommand({isMaster: 1}).arbiterOnly) {
        print("Skipping background validation against test node: " + host +
              " because it is an arbiter and has no data.");
        return {ok: 1};
    }

    print("Running background validation on all collections on test node: " + host);

    // Save a map of namespace to validate cmd results for any cmds that fail so that we can return
    // the results afterwards.
    let failedValidateResults = [];

    // Validate all collections in every database.

    const multitenancyRes = conn.adminCommand({getParameter: 1, multitenancySupport: 1});
    const multitenancy = multitenancyRes.ok && multitenancyRes["multitenancySupport"];

    const cmdObj = multitenancy
        ? {"listDatabasesForAllTenants": 1, "$readPreference": {"mode": "nearest"}}
        : {"listDatabases": 1, "nameOnly": true, "$readPreference": {"mode": "nearest"}};

    const dbs = assert.commandWorked(conn.adminCommand(cmdObj)).databases.map(function(z) {
        return {name: z.name, tenant: z.tenantId};
    });

    conn.adminCommand({configureFailPoint: "crashOnMultikeyValidateFailure", mode: "alwaysOn"});
    for (let dbInfo of dbs) {
        const dbName = dbInfo.name;
        const token = dbInfo.tenant ? _createTenantToken({tenant: dbInfo.tenant}) : undefined;

        try {
            conn._setSecurityToken(token);
            let db = conn.getDB(dbName);

            // TODO (SERVER-25493): Change filter to {type: 'collection'}.
            const listCollRes = assert.commandWorked(db.runCommand({
                "listCollections": 1,
                "nameOnly": true,
                "filter": {$or: [{type: 'collection'}, {type: {$exists: false}}]},
                "$readPreference": {"mode": "nearest"},
            }));
            const collectionNames = new DBCommandCursor(db, listCollRes).map(function(z) {
                return z.name;
            });

            for (let collectionName of collectionNames) {
                let res = conn.getDB(dbName).getCollection(collectionName).runCommand({
                    "validate": collectionName,
                    background: true,
                    "$readPreference": {"mode": "nearest"}
                });

                if ((!res.ok && !isIgnorableErrorFunc(res.codeName)) || (res.valid === false)) {
                    failedValidateResults.push({"ns": dbName + "." + collectionName, "res": res});
                }
            }
        } finally {
            conn._setSecurityToken(undefined);
        }
    }
    conn.adminCommand({configureFailPoint: "crashOnMultikeyValidateFailure", mode: "off"});

    // If any commands failed, format and return an error.
    if (failedValidateResults.length) {
        let errorsArray = [];
        for (let nsAndRes of failedValidateResults) {
            errorsArray.push({"namespace": nsAndRes.ns, "res": nsAndRes.res});
        }

        const heading = "Validate command(s) with {background:true} failed against mongod";
        print(heading + " '" + conn.host + "': \n" + tojson(errorsArray));

        return {ok: 0, error: "Validate failure (search for the following heading): " + heading};
    }

    return {ok: 1};
};

if (topology.type === Topology.kStandalone) {
    let res = validateCollectionsBackgroundThread(topology.mongod);
    assert.commandWorked(
        res,
        () => 'background collection validation against the standalone failed: ' + tojson(res));
} else if (topology.type === Topology.kReplicaSet) {
    const threads = [];
    try {
        for (let replicaMember of topology.nodes) {
            const thread =
                new Thread(validateCollectionsBackgroundThread, replicaMember, isIgnorableError);
            threads.push(thread);
            thread.start();
        }
    } finally {
        // Wait for each thread to finish and gather any errors.
        let gatheredErrors = [];
        const returnData = threads.map(thread => {
            try {
                thread.join();

                // Calling returnData can cause an error thrown in the thread to be thrown again, so
                // we do this in a try-catch block.
                let res = thread.returnData();

                if (!res.ok) {
                    gatheredErrors.push(res);
                }
            } catch (e) {
                gatheredErrors.push(e);
            }
        });

        if (gatheredErrors.length) {
            // eslint-disable-next-line
            throw new Error(
                "Background collection validation was not successful against all replica set " +
                "members: \n" + tojson(gatheredErrors));
        }
    }
} else {
    throw new Error('Unsupported topology configuration: ' + tojson(topology));
}
