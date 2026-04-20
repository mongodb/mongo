/*
 * Auth test for the initReplicatedFastcount command wrapped in an applyOps command, running against
 * a standalone mongod and a replica set.
 * @tags: [
 *   featureFlagReplicatedFastCount,
 *   requires_replication,
 *   requires_fcv_90
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Auth-test the initReplicatedFastCount oplog command, when executed inside an applyOps command.
export function runTest(mongod) {
    const admin = mongod.getDB("admin");
    admin.createUser({user: "admin", pwd: "pass", roles: jsTest.adminUserRoles});
    assert(admin.auth("admin", "pass"));

    // Create roles with database-level permissions for test1 and test2.
    ["test1", "test2"].forEach((dbName) => {
        admin.createRole({
            role: `${dbName}_insert`,
            privileges: [
                // applyOps privilege is needed to send a initReplicatedFastCount command.
                {resource: {cluster: true}, actions: ["applyOps"]},
                {resource: {db: dbName, collection: ""}, actions: ["insert"]},
            ],
            roles: [],
        });
    });

    // user1 has privileges on test1.
    admin.createUser({
        user: "user1",
        pwd: "pass",
        roles: ["test1_insert"],
    });

    // user2 has privileges on test2.
    admin.createUser({
        user: "user2",
        pwd: "pass",
        roles: ["test2_insert"],
    });

    // user3 has no privileges.
    admin.createUser({
        user: "user3",
        pwd: "pass",
        roles: [],
    });

    // user4 is a __system user with privileges on the admin database.
    admin.createUser({
        user: "user4",
        pwd: "pass",
        roles: ["__system"],
    });

    admin.logout();

    const runAuthTest = function ({user, dbName, expectedError}) {
        admin.auth(user, "pass");

        const db = admin.getSiblingDB(dbName);
        // Run initReplicatedFastCount command as part of an applyOps command. This is necessary
        // because there is no user-facing initReplicatedFastCount command in the server.
        const applyOpsCmd = {
            applyOps: [
                {
                    op: "c",
                    ns: `${dbName}.$cmd`,
                    o: {
                        "initReplicatedFastCount": 1,
                    },
                    // Don't need an o2 since the op will be rejected.
                },
            ],
        };

        if (expectedError) {
            assert.commandFailedWithCode(db.runCommand(applyOpsCmd), [expectedError]);
        } else {
            assert.commandWorked(db.runCommand(applyOpsCmd));
        }
        admin.logout();
    };

    // user1 and user2 have applyOps privilege (from their roles), so the applyOps auth check
    // passes. The initReplicatedFastCount command is not a registered command, so
    // CommandHelpers::findCommand returns null and we get FailedToParse regardless of which
    // database the op targets.
    runAuthTest({user: "user1", dbName: "test1", expectedError: ErrorCodes.FailedToParse});
    runAuthTest({user: "user1", dbName: "test2", expectedError: ErrorCodes.FailedToParse});
    runAuthTest({user: "user2", dbName: "test1", expectedError: ErrorCodes.FailedToParse});
    runAuthTest({user: "user2", dbName: "test2", expectedError: ErrorCodes.FailedToParse});

    // user3 has no privileges -> Unauthorized (fails the applyOps cluster-level check).
    runAuthTest({user: "user3", dbName: "test1", expectedError: ErrorCodes.Unauthorized});
    runAuthTest({user: "user3", dbName: "test2", expectedError: ErrorCodes.Unauthorized});

    // user4 (__system) has all privileges -> FailedToParse on admin.
    runAuthTest({user: "user4", dbName: "admin", expectedError: ErrorCodes.FailedToParse});
}

const mongod = MongoRunner.runMongod({auth: ""});
runTest(mongod);
MongoRunner.stopMongod(mongod);

const replTest = new ReplSetTest({name: jsTestName(), nodes: 2, keyFile: "jstests/libs/key1"});
replTest.startSet();
replTest.initiate();
runTest(replTest.getPrimary());
replTest.stopSet();
