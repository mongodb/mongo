// Auth-test the truncateRange oplog command, when executed inside an applyOps command.
export function runTest(mongod) {
    const roleName = (dbName, collName) => `${dbName}_${collName}_remove`;

    const databases = ["test1", "test2"];
    const collections = ["coll1", "coll2"];

    let docs = [];
    for (let i = 0; i < 10; ++i) {
        docs.push({_id: i});
    }

    const admin = mongod.getDB("admin");
    admin.createUser({user: "admin", pwd: "pass", roles: jsTest.adminUserRoles});
    assert(admin.auth("admin", "pass"));

    // Set up test databases and collections.
    databases.forEach((dbName) => {
        collections.forEach((collName) => {
            // truncateRange only works when there is a clustered index, so create one.
            assert.commandWorked(
                mongod.getDB(dbName).createCollection(collName, {clusteredIndex: {key: {_id: 1}, unique: true}}),
            );
            assert.commandWorked(mongod.getDB(dbName).getCollection(collName).insertMany(docs));

            admin.createRole({
                role: roleName(dbName, collName),
                privileges: [
                    // applyOps privilege is needed to send a truncateRange command.
                    {resource: {cluster: true}, actions: ["applyOps"]},
                    {resource: {db: dbName, collection: collName}, actions: ["remove"]},
                ],
                roles: [],
            });
        });
    });

    // Create various test users.
    // user1 has privileges on some of the collections.
    admin.createUser({
        user: "user1",
        pwd: "pass",
        roles: [roleName(databases[0], collections[0])],
    });

    // user2 has privileges on some other collections.
    admin.createUser({
        user: "user2",
        pwd: "pass",
        roles: [roleName(databases[0], collections[1]), roleName(databases[1], collections[0])],
    });

    // user3 has no privileges.
    admin.createUser({
        user: "user3",
        pwd: "pass",
        roles: [],
    });

    // user4 is a system user and is allowed to do everything, but still cannot run an applyOps containing a 'truncateRange'.
    admin.createUser({
        user: "user4",
        pwd: "pass",
        roles: ["__system"],
    });

    admin.logout();

    const runAuthTest = function (test) {
        // Run test for the different databases and collections.
        databases.forEach((dbName) => {
            collections.forEach((collName) => {
                assert(admin.auth("admin", "pass"));
                const db = admin.getSiblingDB(dbName);
                const docs = db.getCollection(collName).find().sort({_id: 1}).showRecordId().toArray();
                admin.logout();

                admin.auth(test.user, "pass");

                // Run truncateRange command as part of an applyOps command. This is necessary because there
                // is no user-facing truncateRange command in the server.
                const applyOpsCmd = {
                    applyOps: [
                        {
                            op: "c",
                            ns: `${test.dbName}.$cmd`,
                            o: {
                                "truncateRange": `${test.dbName}.${test.collName}`,
                                "minRecordId": docs[0].$recordId,
                                "maxRecordId": docs[1].$recordId,
                                "bytesDeleted": 1, // just a placeholder
                                "docsDeleted": 1,
                            },
                        },
                    ],
                };

                if (test.expectedError) {
                    assert.commandFailedWithCode(db.runCommand(applyOpsCmd), [test.expectedError]);
                } else {
                    assert.commandWorked(db.runCommand(applyOpsCmd));
                }
                admin.logout();
            });
        });
    };

    // The following commands run into the "Unrecognized command in op" error in oplog_application_checks.cpp.
    runAuthTest({user: "user1", expectedError: ErrorCodes.FailedToParse}); // Partial privileges.
    runAuthTest({user: "user2", expectedError: ErrorCodes.FailedToParse}); // Partial privileges.
    runAuthTest({user: "user4", expectedError: ErrorCodes.FailedToParse}); // __system privileges.

    // The following commands trigger an "Unauthorized" error.
    runAuthTest({user: "user3", expectedError: ErrorCodes.Unauthorized}); // No privileges.
    runAuthTest({user: "doesNotExist", expectedError: ErrorCodes.Unauthorized}); // User does not exist.
}
