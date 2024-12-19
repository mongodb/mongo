import {assertWriteConcernError} from "jstests/libs/write_concern_util.js";

// Each entry in the returned array contains a command whose write concern behavior needs to be
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
//
//      admin: <bool>,                   // Optional. If set to true, req will be executed as
//                                       // an admin command.
// }

function runCommandTest(self, coll, conn, preSetup) {
    const dbName = coll.getDB().getName();

    // Drop test collection.
    coll.drop();
    assert.eq(0, coll.find().itcount(), "test collection not empty");

    jsTest.log("Testing command in " + self.setupType + " setup: " + tojson(self.req));

    // Create environment for command to run in.
    if (preSetup) {
        preSetup();
    }
    self.setupFunc(conn);

    // Provide a small wtimeout that we expect to time out.
    self.req.writeConcern = {w: 3, wtimeout: 1000};

    // We check the error code of 'res' in the 'confirmFunc'.
    const res = self.admin ? conn.adminCommand(self.req) : conn.getDB(dbName).runCommand(self.req);

    try {
        // Tests that the command receives a write concern error. If we don't wait for write
        // concern on noop writes then we won't get a write concern error.
        assertWriteConcernError(res);
        // Validate post-conditions of the commands.
        jsTest.log("Command returned " + tojson(res));
        self.confirmFunc(res);
    } catch (e) {
        // Make sure that we print out the response.
        printjson(res);
        throw e;
    }
}

export function getWriteCommands(coll, setupType, preSetup) {
    const db = coll.getDB();
    const dbName = db.getName();
    const collName = coll.getName();
    const commands = [];
    assert(setupType === "rs" || setupType === "sharding-sharded" ||
               setupType === "sharding-unsharded",
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

    if (setupType !== "sharding-unsharded" && setupType !== "sharding-sharded") {
        // TODO SERVER-98461 Fix missing WCE; remove the above conditional.
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

        // TODO SERVER-98461 Fix missing WCE; remove the above conditional.
        // 'update' where the update has already been done.
        commands.push({
            checkWriteConcern: (res) => {
                assertWriteConcernError(res);
            },
            req: {update: collName, updates: [{q: {a: 1}, u: {$set: {c: 2}}}]},
            setupFunc: () => {
                assert.commandWorked(coll.insert({a: 1}));
                assert.commandWorked(coll.update({a: 1}, {$set: {c: 2}}));
            },
            confirmFunc: (res) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                if (setupType !== "sharding-sharded") {
                    assert.eq(res.n, 1);
                } else {
                    // TODO SERVER-98462 Remove this test conditional.
                    assert.eq(res.n, 0);
                }
                assert.eq(res.nModified, 0);
                assert.eq(coll.find().itcount(), 1);
                assert.eq(coll.count({a: 1, c: 2}), 1);
            },
        });

        // TODO SERVER-98461 Fix missing WCE; remove the above conditional.
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

        // TODO SERVER-98461 Fix missing WCE; remove the above conditional.
        // update documents across multiple shards
        commands.push({
            req: {
                update: collName,
                updates: [{q: {_id: {$gt: -21}}, u: {$set: {a: 1}}, multi: true}]
            },
            setupFunc: function() {
                assert.commandWorked(coll.insert([{_id: -22}, {_id: -20}, {_id: 20}, {_id: 21}]));
            },
            confirmFunc: (res) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                if (setupType == "rs") {
                    assert.eq(coll.find({a: 1}).toArray().length, 3);
                } else {
                    // TODO SERVER-98462 Remove this test conditional.
                    assert.eq(setupType, "sharding-sharded");
                    assert.eq(coll.find({a: 1}).toArray().length, 0);
                }
            },
        });

        // TODO SERVER-98461 Fix missing WCE; remove the above conditional.
        // update, multiple documents
        commands.push({
            req: {update: collName, updates: [{q: {a: 1}, u: {$set: {d: 2}}, multi: true}]},
            setupFunc: () => {
                assert.commandWorked(coll.insert({a: 1, aa: 11}));
                assert.commandWorked(coll.insert({a: 1, aa: 22}));
            },
            confirmFunc: (res) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assert(res.ok);
                if (setupType === "rs") {
                    assert.eq(res.n, 2);
                    assert.eq(res.nModified, 2);
                } else {
                    // TODO SERVER-98462 Remove this test conditional.
                    assert.eq(setupType, "sharding-sharded");
                    assert.eq(res.n, 0);
                    assert.eq(res.nModified, 0);
                }
                assert.eq(coll.count({a: 1}), 2);
            },
        });

        // TODO SERVER-98461 Fix missing WCE; remove the above conditional.
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

        // All voting data bearing nodes are not up for this test. So 'createIndexes' command
        // can't succeed with the default index commitQuorum value "votingMembers". So, running
        // createIndexes cmd using commit quorum "majority".
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
                if (setupType !== "sharding-sharded") {
                    assert.eq(details.numIndexesBefore, details.numIndexesAfter);
                    assert.eq(details.note, 'all indexes already exist');
                } else {
                    // TODO SERVER-98462 Remove this test conditional.
                    assert.eq(details.numIndexesBefore, details.numIndexesAfter - 1);
                }
            },
        });
    }

    // 'dropIndexes'
    commands.push({
        req: {
            dropIndexes: collName,
            index: "a_1",
        },
        setupFunc: () => {
            assert.commandWorked(coll.insert({a: "a"}));
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
        },
    });

    if (setupType !== "sharding-unsharded" && setupType !== "sharding-sharded") {
        // TODO SERVER-98602 fix missing WCE; remove above condition
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

        // TODO SERVER-98602 fix missing WCE; remove above condition
        // 'findAndModify' where the update has already been done.
        commands.push({
            req: {findAndModify: collName, query: {a: 1}, update: {$set: {c: 2}}},
            setupFunc: () => {
                assert.commandWorked(coll.insert({a: 1}));
                assert.commandWorkedIgnoringWriteConcernErrors(db.runCommand(
                    {findAndModify: collName, query: {a: 1}, update: {$set: {c: 2}}}));
            },
            confirmFunc: (res) => {
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                if (setupType === "rs") {
                    assert.eq(res.lastErrorObject.updatedExisting, true);
                } else {
                    // TODO SERVER-98603 remove this test conditional
                    assert.eq(setupType, "sharding-sharded");
                    assert.eq(res.lastErrorObject.updatedExisting, false);
                }
                assert.eq(coll.find().itcount(), 1);
                assert.eq(coll.count({a: 1, c: 2}), 1);
            },
        });
    }

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

    // TODO SERVER-98602 Fix missing WCE; remove the "sharding-(un)sharded" conditions below.
    if (setupType.startsWith("sharding") && setupType !== "sharding-unsharded" &&
        setupType !== "sharding-sharded") {
        // 'findAndModify' causing a unique index key violation. Only works in a sharded cluster.
        commands.push({
            req: {findAndModify: collName, query: {value: {$lt: 3}}, update: {$set: {value: 3}}},
            setupFunc: (shell) => {
                coll.drop();
                coll.createIndex({value: 1}, {unique: true});
                assert.commandWorked(
                    shell.adminCommand({shardCollection: coll.getFullName(), key: {value: 1}}));
                assert.commandWorked(coll.insert({value: 1}));
                assert.commandWorked(coll.insert({value: 2}));
                assert.commandWorked(coll.insert({value: 3}));
            },
            confirmFunc: (res) => {
                if (setupType === "sharding-sharded") {
                    // TODO SERVER-98462 Remove this test conditional.
                    assert.eq(res.ok, 1);
                } else {
                    assert.commandFailedWithCode(res, ErrorCodes.IllegalOperation);
                }
                assert.eq(coll.count({value: 1}), 1);
                assert.eq(coll.count({value: 2}), 1);
                assert.eq(coll.count({value: 3}), 1);
            },
        });
    }

    if (setupType !== "sharding-unsharded" && setupType !== "sharding-sharded") {
        // TODO SERVER-98602 no WCE for unsharded collection on sharded cluster
        // 'findAndModify' with immutable field error.
        commands.push({
            req: {findAndModify: collName, query: {_id: 1}, update: {$set: {_id: 2}}},
            setupFunc: () => {
                assert.commandWorked(coll.insert({_id: 1}));
            },
            confirmFunc: (res) => {
                if (setupType === "sharding-sharded") {
                    // TODO SERVER-98462 Remove this test conditional.
                    assert.eq(res.ok, 1);
                } else {
                    assert.eq(setupType, "rs");
                    assert.commandFailedWithCode(res, ErrorCodes.ImmutableField);
                }
                assert.eq(coll.find().itcount(), 1);
                assert.eq(coll.count({_id: 1}), 1);
            },
        });

        // TODO SERVER-98602 no WCE for unsharded collection on sharded cluster
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
    }

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
            assert.commandFailedWithCode(res, ErrorCodes.NamespaceNotFound);
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
            assert.commandFailedWithCode(res, ErrorCodes.NamespaceExists);
        },
    });

    if (setupType !== "sharding-unsharded" && setupType !== "sharding-sharded") {
        // TODO SERVER-98461 Fix missing WCE; remove the above conditional.
        // 'insert' where the document with the same _id has already been inserted.
        commands.push({
            req: {insert: collName, documents: [{_id: 10}]},
            setupFunc: () => {
                assert.commandWorked(coll.insert({_id: 10}));
            },
            confirmFunc: (res) => {
                assertWriteConcernError(res);
                if (setupType !== "sharding-sharded") {
                    assert.eq(res.n, 0);
                    assert.eq(res.writeErrors[0].code, ErrorCodes.DuplicateKey);
                } else {
                    // TODO SERVER-98462 Remove this test conditional.
                    assert.eq(setupType, "sharding-sharded");
                    assert.eq(res.n, 1);
                    assert.eq(res.writeErrors, undefined);
                }
                assert.eq(coll.count({_id: 10}), 1);
            },
        });
    }

    // 'renameCollection'
    commands.push({
        admin: true,
        req: {renameCollection: dbName + "." + collName, to: dbName + "." + collName + "1"},
        setupFunc: () => {
            db[collName + "1"].drop();
            assert.commandWorked(coll.insert({_id: 1}));
        },
        confirmFunc: (res) => {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            assert(db[collName + "1"].drop());
        },
    });

    // 'renameCollection' with dropTarget: true but target does not exist.
    commands.push({
        admin: true,
        req: {
            renameCollection: dbName + "." + collName,
            to: dbName + "." + collName + "1",
            dropTarget: true
        },
        setupFunc: () => {
            db[collName + "1"].drop();
            assert.commandWorked(coll.insert({_id: 1}));
        },
        confirmFunc: (res) => {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            assert(db[collName + "1"].drop());
        },
    });

    if (setupType !== "sharding-unsharded" && setupType !== "sharding-sharded") {
        // TODO SERVER-98461 Fix missing WCE; remove the above conditional.
        // timeseries insert
        commands.push({
            req: {
                insert: collName,
                documents: [{timestamp: ISODate('2024-01-01'), metadata: "time series doc"}]
            },
            setupFunc: () => {
                db[collName].drop();
                assert.commandWorked(db.createCollection(collName, {
                    timeseries: {timeField: "timestamp", metaField: "metadata"},
                    expireAfterSeconds: 1000,
                }));
            },
            confirmFunc: (res) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert(res.ok);
            },
        });

        // TODO SERVER-98461 Fix missing WCE; remove the above conditional.
        // timeseries update
        commands.push({
            req: {
                update: collName,
                updates: [{
                    q: {metadata: "time series doc"},
                    u: {$set: {metadata: "time series doc, too"}},
                }]
            },
            setupFunc: () => {
                db[collName].drop();
                assert.commandWorked(db.createCollection(collName, {
                    timeseries: {timeField: "timestamp", metaField: "metadata"},
                    expireAfterSeconds: 1000,
                }));
                assert.commandWorked(db[collName].insert(
                    {timestamp: ISODate('2024-01-01'), metadata: "time series doc"},
                    {writeConcern: {w: "majority"}}));
            },
            confirmFunc: (res) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert(res.ok);
            },
        });

        // TODO SERVER-98461 Fix missing WCE; remove the above conditional.
        // timeseries update, doc does not exist
        commands.push({
            req: {update: collName, updates: [{q: {metadata: "DNE"}, u: {$set: {metadata: "DE"}}}]},
            setupFunc: () => {
                db[collName].drop();
                assert.commandWorked(db.createCollection(collName, {
                    timeseries: {timeField: "timestamp", metaField: "metadata"},
                    expireAfterSeconds: 1000,
                }));
            },
            confirmFunc: (res) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert(res.ok);
            },
        });

        // TODO SERVER-98461 Fix missing WCE; remove the above conditional.
        // timeseries update, update has already been done
        commands.push({
            req: {
                update: collName,
                updates: [{
                    q: {metadata: "time series doc 1"},
                    u: {$set: {metadata: "time series doc 1, too"}},
                }]
            },
            setupFunc: () => {
                db[collName].drop();
                assert.commandWorked(db.createCollection(collName, {
                    timeseries: {timeField: "timestamp", metaField: "metadata"},
                    expireAfterSeconds: 1000,
                }));
                assert.commandWorked(db[collName].insert(
                    {timestamp: ISODate('2024-01-01'), metadata: "time series doc 1"},
                    {writeConcern: {w: "majority"}}));
                assert.commandWorked(
                    db[collName].update({metadata: "time series doc 1"},
                                        {$set: {metadata: "time series doc 1, too"}},
                                        {multi: true}));
            },
            confirmFunc: (res) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert(res.ok);
            },
        });

        // TODO SERVER-98461 Fix missing WCE; remove the above conditional.
        // time series delete
        commands.push({
            req: {delete: collName, deletes: [{q: {metadata: "time series doc 2"}, limit: 1}]},
            setupFunc: () => {
                db[collName].drop();
                assert.commandWorked(db.createCollection(collName, {
                    timeseries: {timeField: "timestamp", metaField: "metadata"},
                    expireAfterSeconds: 1000,
                }));
                assert.commandWorked(db[collName].insert(
                    {timestamp: ISODate('2024-01-01'), metadata: "time series doc 2"},
                    {writeConcern: {w: "majority"}}));
            },
            confirmFunc: (res) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert(res.ok);
            },
        });

        // TODO SERVER-98461 Fix missing WCE; remove the above conditional.
        // time series delete, document does not exist
        commands.push({
            req: {delete: collName, deletes: [{q: {metadata: "time series doc 3"}, limit: 1}]},
            setupFunc: () => {
                db[collName].drop();
                assert.commandWorked(db.createCollection(collName, {
                    timeseries: {timeField: "timestamp", metaField: "metadata"},
                    expireAfterSeconds: 1000,
                }));
                assert.commandWorked(db[collName].insert(
                    {timestamp: ISODate('2024-01-01'), metadata: "time series doc 3"},
                    {writeConcern: {w: "majority"}}));
                assert.commandWorked(db[collName].remove({metadata: "time series doc 3"}));
            },
            confirmFunc: (res) => {
                assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
                assert(res.ok, 0);
            },
        });
    }

    // Attach a 'run' function to each command for the actual test execution.
    commands.forEach((cmd) => {
        // Validate that all commands have proper definitions.
        ['req', 'setupFunc', 'confirmFunc'].forEach((field) => {
            assert(cmd.hasOwnProperty(field),
                   `command does not have required field '${field}': ${tojson(cmd)}`);
        });

        // Attach a 'run' function to each command for the actual test execution.
        let self = cmd;
        cmd.run = (coll, conn) => runCommandTest(self, coll, conn, preSetup);
        cmd.setupType = setupType;
    });

    return commands;
}

export function getWriteCommandsForShardedCollection(coll, preSetup) {
    const db = coll.getDB();
    const dbName = db.getName();
    const collName = coll.getName();
    const commands = [];
    const ns = dbName + '.' + collName;

    /**
     * TODO SERVER-98461 no WCE in sharded collection

    // update shard key value, upsert
    commands.push({
        req: {update: collName, updates: [{q: {_id: -1}, u: {_id: -2}, upsert: true}]},
        setupFunc: function() {
            assert.commandWorked(coll.insert({_id: -100}));
        },
        confirmFunc: (res) => {
            assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
            assert(res.ok);
            assert.eq(coll.count({_id: -2}), 0);
        },
    });

    // update shard key value, within chunk
    commands.push({
        req: {update: collName, updates: [{q: {_id: -1}, u: {_id: -2}}]},
        setupFunc: function() {
            assert.commandWorked(coll.insert({_id: -1}));
        },
        confirmFunc: (res) => {
            assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
            assert(res.ok);
            assert.eq(coll.count({_id: -1}), 1);
        },
    });

    // update shard key value, cross-shard.
    commands.push({
        req: {update: collName, updates: [{q: {_id: 10}, u: {_id: -10}}]},
        setupFunc: function() {
            assert.commandWorked(coll.insert({_id: 10}));
        },
        confirmFunc: (res) => {
            assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
            assert(res.ok);
            assert.eq(coll.count({_id: 10}), 1);
        },
    });

    // update, no shard key value.
    commands.push({
        req: {update: collName, updates: [{q: {a: 1}, u: {$set: {b: 1}}}]},
        setupFunc: function() {
            assert.commandWorked(coll.insert({keyField1: 5, a: 1}));
        },
        confirmFunc: (res) => {
            assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
            assert(res.ok);
            assert.eq(coll.count({a: 1, b: 1}), 0);
            assert.eq(coll.count({keyField1: 5}), 1);
        },
    });

    // delete, no shard key value.
    commands.push({
        req: {delete: collName, deletes: [{q: {c: 2}, limit: 1}]},
        setupFunc: function() {
            assert.commandWorked(coll.insert({keyField1: 6, c: 1}));
        },
        confirmFunc: (res) => {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            assert(res.ok);
            assert.eq(coll.count({keyField1: 6}), 1);
        },
    });

    // findAndModify, no shard key value.
    commands.push({
        req: {findAndModify: collName, query: {d: 1}, update: {keyField1: 8, d: 2}},
        setupFunc: function() {
            assert.commandWorked(coll.insert({keyField1: 7, d: 1}));
        },
        confirmFunc: (res) => {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            assert(res.ok);
            assert.eq(coll.count({keyField1: 7}), 1);
        },
    });
    *
  */

    // Attach a 'run' function to each command for the actual test execution.
    commands.forEach((cmd) => {
        // Validate that all commands have proper definitions.
        ['req', 'setupFunc', 'confirmFunc'].forEach((field) => {
            assert(cmd.hasOwnProperty(field),
                   `command does not have required field '${field}': ${tojson(cmd)}`);
        });

        // Attach a 'run' function to each command for the actual test execution.
        let self = cmd;
        cmd.run = (coll, conn) => runCommandTest(self, coll, conn, preSetup);
        cmd.setupType = "sharding-sharded";
    });

    return commands;
}
