import {assertWriteConcernError} from "jstests/libs/write_concern_util.js";

// Each entry in the returned array contains a command whose noop write concern behavior needs to be
// tested. Entries have the following structure:
// {
//      req: <object>,                   // Command request object that will result in a noop
//                                       // write after the setup function is called.
//
//      setupFunc: <function()>,         // Function to run to ensure that the request is a
//                                       // noop.
//
//      confirmFunc: <function(res)>,    // Function to run after the command is run to ensure
//                                       // that it executed properly. Accepts the result of
//                                       // the noop request to validate it.
// }
export function getNoopWriteCommands(coll, setupType) {
    const db = coll.getDB();
    const collName = coll.getName();
    const commands = [];

    assert(setupType === "rs" || setupType === "sharding",
           `invalid setupType '${setupType}' used for tests`);

    // 'applyOps' where the update has already been done.
    commands.push({
        req: {applyOps: [{op: "u", ns: coll.getFullName(), o: {_id: 1}, o2: {_id: 1}}]},
        setupFunc: () => {
            assert.commandWorked(coll.insert({_id: 1}));
        },
        confirmFunc: (res) => {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            assert.eq(res.applied, 1);
            assert.eq(res.results[0], true);
            assert.eq(coll.find().itcount(), 1);
            assert.eq(coll.count({_id: 1}), 1);
        },
    });

    // 'update' where the document to update does not exist.
    commands.push({
        req: {update: collName, updates: [{q: {a: 1}, u: {b: 2}}]},
        setupFunc: () => {
            assert.commandWorked(coll.insert({a: 1}));
            assert.commandWorked(coll.update({a: 1}, {b: 2}));
        },
        confirmFunc: (res) => {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            assert.eq(res.n, 0);
            assert.eq(res.nModified, 0);
            assert.eq(coll.find().itcount(), 1);
            assert.eq(coll.count({b: 2}), 1);
        },
    });

    // 'update' where the update has already been done.
    commands.push({
        checkWriteConcern: (res) => {
            assertWriteConcernError(res);
        },
        req: {update: collName, updates: [{q: {a: 1}, u: {$set: {b: 2}}}]},
        setupFunc: () => {
            assert.commandWorked(coll.insert({a: 1}));
            assert.commandWorked(coll.update({a: 1}, {$set: {b: 2}}));
        },
        confirmFunc: (res) => {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            assert.eq(res.n, 1);
            assert.eq(res.nModified, 0);
            assert.eq(coll.find().itcount(), 1);
            assert.eq(coll.count({a: 1, b: 2}), 1);
        },
    });

    // 'update' with immutable field error.
    commands.push({
        checkWriteConcern: (res) => {
            assertWriteConcernError(res);
        },
        req: {update: collName, updates: [{q: {_id: 1}, u: {$set: {_id: 2}}}]},
        setupFunc: () => {
            assert.commandWorked(coll.insert({_id: 1}));
        },
        confirmFunc: (res) => {
            assert.eq(res.n, 0);
            assert.eq(res.nModified, 0);
            assert.eq(coll.count({_id: 1}), 1);
        },
    });

    // 'delete' where the document to delete does not exist.
    commands.push({
        req: {delete: collName, deletes: [{q: {a: 1}, limit: 1}]},
        setupFunc: () => {
            assert.commandWorked(coll.insert({a: 1}));
            assert.commandWorked(coll.remove({a: 1}));
        },
        confirmFunc: (res) => {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            assert.eq(res.n, 0);
            assert.eq(coll.count({a: 1}), 0);
        },
    });

    // 'createIndexes' where the index has already been created.
    // All voting data bearing nodes are not up for this test. So 'createIndexes' command can't
    // succeed with the default index commitQuorum value "votingMembers". So, running createIndexes
    // cmd using commit quorum "majority".
    commands.push({
        req: {
            createIndexes: collName,
            indexes: [{key: {a: 1}, name: "a_1"}],
            commitQuorum: "majority"
        },
        setupFunc: () => {
            assert.commandWorked(coll.insert({a: 1}));
            assert.commandWorkedIgnoringWriteConcernErrors(db.runCommand({
                createIndexes: collName,
                indexes: [{key: {a: 1}, name: "a_1"}],
                commitQuorum: "majority"
            }));
        },
        confirmFunc: (res) => {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            let details = res;
            if ("raw" in details) {
                const raw = details.raw;
                details = raw[Object.keys(raw)[0]];
            }
            assert.eq(details.numIndexesBefore, details.numIndexesAfter);
            assert.eq(details.note, 'all indexes already exist');
        },
    });

    // 'findAndModify' where the document to update does not exist.
    commands.push({
        req: {findAndModify: collName, query: {a: 1}, update: {b: 2}},
        setupFunc: () => {
            assert.commandWorked(coll.insert({a: 1}));
            assert.commandWorkedIgnoringWriteConcernErrors(
                db.runCommand({findAndModify: collName, query: {a: 1}, update: {b: 2}}));
        },
        confirmFunc: (res) => {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            assert.eq(res.lastErrorObject.updatedExisting, false);
            assert.eq(coll.find().itcount(), 1);
            assert.eq(coll.count({b: 2}), 1);
        },
    });

    // 'findAndModify' where the update has already been done.
    commands.push({
        req: {findAndModify: collName, query: {a: 1}, update: {$set: {b: 2}}},
        setupFunc: () => {
            assert.commandWorked(coll.insert({a: 1}));
            assert.commandWorkedIgnoringWriteConcernErrors(
                db.runCommand({findAndModify: collName, query: {a: 1}, update: {$set: {b: 2}}}));
        },
        confirmFunc: (res) => {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            assert.eq(res.lastErrorObject.updatedExisting, true);
            assert.eq(coll.find().itcount(), 1);
            assert.eq(coll.count({a: 1, b: 2}), 1);
        },
    });

    // 'findAndModify' causing a unique index key violation. Only works in a replica set.
    if (setupType === "rs") {
        commands.push({
            req: {findAndModify: collName, query: {value: {$lt: 3}}, update: {$set: {value: 3}}},
            setupFunc: (shell) => {
                coll.createIndex({value: 1}, {unique: true});
                assert.commandWorked(coll.insert({value: 1}));
                assert.commandWorked(coll.insert({value: 2}));
                assert.commandWorked(coll.insert({value: 3}));
            },
            confirmFunc: (res) => {
                assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey);
                assert.eq(coll.count({value: 1}), 1);
                assert.eq(coll.count({value: 2}), 1);
                assert.eq(coll.count({value: 3}), 1);
            },
        });
    }

    // 'findAndModify' causing a unique index key violation. Only works in a sharded cluster.
    if (setupType === "sharding") {
        commands.push({
            req: {findAndModify: collName, query: {value: {$lt: 3}}, update: {$set: {value: 3}}},
            setupFunc: (shell) => {
                coll.createIndex({value: 1}, {unique: true});
                assert.commandWorked(
                    shell.adminCommand({shardCollection: coll.getFullName(), key: {value: 1}}));
                assert.commandWorked(coll.insert({value: 1}));
                assert.commandWorked(coll.insert({value: 2}));
                assert.commandWorked(coll.insert({value: 3}));
            },
            confirmFunc: (res) => {
                assert.commandFailedWithCode(res, ErrorCodes.IllegalOperation);
                assert.eq(coll.count({value: 1}), 1);
                assert.eq(coll.count({value: 2}), 1);
                assert.eq(coll.count({value: 3}), 1);
            },
        });
    }

    // 'findAndModify' with immutable field error.
    commands.push({
        req: {findAndModify: collName, query: {_id: 1}, update: {$set: {_id: 2}}},
        setupFunc: () => {
            assert.commandWorked(coll.insert({_id: 1}));
        },
        confirmFunc: (res) => {
            assert.commandFailedWithCode(res, ErrorCodes.ImmutableField);
            assert.eq(coll.find().itcount(), 1);
            assert.eq(coll.count({_id: 1}), 1);
        },
    });

    // 'findAndModify' where the document to delete does not exist.
    commands.push({
        req: {findAndModify: collName, query: {a: 1}, remove: true},
        setupFunc: () => {
            assert.commandWorked(coll.insert({a: 1}));
            assert.commandWorked(coll.remove({a: 1}));
        },
        confirmFunc: (res) => {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            assert.eq(res.lastErrorObject.n, 0);
        },
    });

    // 'dropDatabase' where the database has already been dropped.
    commands.push({
        req: {dropDatabase: 1},
        setupFunc: () => {
            assert.commandWorked(coll.insert({a: 1}));
            assert.commandWorkedIgnoringWriteConcernErrors(db.runCommand({dropDatabase: 1}));
        },
        confirmFunc: (res) => {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
        },
    });

    // 'drop' where the collection has already been dropped.
    commands.push({
        req: {drop: collName},
        setupFunc: () => {
            assert.commandWorked(coll.insert({a: 1}));
            assert.commandWorkedIgnoringWriteConcernErrors(db.runCommand({drop: collName}));
        },
        confirmFunc: (res) => {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
        },
    });

    // 'create' where the collection has already been created.
    commands.push({
        req: {create: collName},
        setupFunc: () => {
            assert.commandWorkedIgnoringWriteConcernErrors(db.runCommand({create: collName}));
        },
        confirmFunc: (res) => {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
        },
    });

    // 'insert' where the document with the same _id has already been inserted.
    commands.push({
        req: {insert: collName, documents: [{_id: 1}]},
        setupFunc: () => {
            assert.commandWorked(coll.insert({_id: 1}));
        },
        confirmFunc: (res) => {
            assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
            assert.eq(res.n, 0);
            assert.eq(res.writeErrors[0].code, ErrorCodes.DuplicateKey);
            assert.eq(coll.count({_id: 1}), 1);
        },
    });

    commands.forEach((cmd) => {
        // Validate that all commands have proper definitions.
        ['req', 'setupFunc', 'confirmFunc'].forEach((field) => {
            assert(cmd.hasOwnProperty(field),
                   `command does not have required field '${field}': ${tojson(cmd)}`);
        });

        // Attach a 'run' function to each command for the actual test execution.
        let self = cmd;
        cmd.run = (dbName, coll, shell) => {
            // Drop test collection.
            coll.drop();
            assert.eq(0, coll.find().itcount(), "test collection not empty");

            jsTest.log("Testing command: " + tojson(self.req));

            // Create environment for command to run in.
            self.setupFunc(shell);

            // Provide a small wtimeout that we expect to time out.
            self.req.writeConcern = {w: 3, wtimeout: 1000};

            // We check the error code of 'res' in the 'confirmFunc'.
            const res = "bulkWrite" in self.req ? shell.adminCommand(self.req)
                                                : shell.getDB(dbName).runCommand(self.req);

            try {
                // Tests that the command receives a write concern error. If we don't wait for write
                // concern on noop writes then we won't get a write concern error.
                assertWriteConcernError(res);
                // Validate post-conditions of the commands.
                self.confirmFunc(res);
            } catch (e) {
                // Make sure that we print out the response.
                printjson(res);
                throw e;
            }
        };
    });

    return commands;
}
