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
function getNoopWriteCommands(coll) {
    const db = coll.getDB();
    const collName = coll.getName();
    const commands = [];

    // 'applyOps' where the update has already been done.
    commands.push({
        req: {applyOps: [{op: "u", ns: coll.getFullName(), o: {_id: 1}, o2: {_id: 1}}]},
        setupFunc: function() {
            assert.commandWorked(coll.insert({_id: 1}));
        },
        confirmFunc: function(res) {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            assert.eq(res.applied, 1);
            assert.eq(res.results[0], true);
            assert.eq(coll.find().itcount(), 1);
            assert.eq(coll.count({_id: 1}), 1);
        }
    });

    // 'update' where the document to update does not exist.
    commands.push({
        req: {update: collName, updates: [{q: {a: 1}, u: {b: 2}}]},
        setupFunc: function() {
            assert.commandWorked(coll.insert({a: 1}));
            assert.commandWorked(coll.update({a: 1}, {b: 2}));
        },
        confirmFunc: function(res) {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            assert.eq(res.n, 0);
            assert.eq(res.nModified, 0);
            assert.eq(coll.find().itcount(), 1);
            assert.eq(coll.count({b: 2}), 1);
        }
    });

    // 'update' where the update has already been done.
    commands.push({
        req: {update: collName, updates: [{q: {a: 1}, u: {$set: {b: 2}}}]},
        setupFunc: function() {
            assert.commandWorked(coll.insert({a: 1}));
            assert.commandWorked(coll.update({a: 1}, {$set: {b: 2}}));
        },
        confirmFunc: function(res) {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            assert.eq(res.n, 1);
            assert.eq(res.nModified, 0);
            assert.eq(coll.find().itcount(), 1);
            assert.eq(coll.count({a: 1, b: 2}), 1);
        }
    });

    // 'delete' where the document to delete does not exist.
    commands.push({
        req: {delete: collName, deletes: [{q: {a: 1}, limit: 1}]},
        setupFunc: function() {
            assert.commandWorked(coll.insert({a: 1}));
            assert.commandWorked(coll.remove({a: 1}));
        },
        confirmFunc: function(res) {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            assert.eq(res.n, 0);
            assert.eq(coll.count({a: 1}), 0);
        }
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
        setupFunc: function() {
            assert.commandWorked(coll.insert({a: 1}));
            assert.commandWorkedIgnoringWriteConcernErrors(db.runCommand({
                createIndexes: collName,
                indexes: [{key: {a: 1}, name: "a_1"}],
                commitQuorum: "majority"
            }));
        },
        confirmFunc: function(res) {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            let details = res;
            if ("raw" in details) {
                const raw = details.raw;
                details = raw[Object.keys(raw)[0]];
            }
            assert.eq(details.numIndexesBefore, details.numIndexesAfter);
            assert.eq(details.note, 'all indexes already exist');
        }
    });

    // 'findAndModify' where the document to update does not exist.
    commands.push({
        req: {findAndModify: collName, query: {a: 1}, update: {b: 2}},
        setupFunc: function() {
            assert.commandWorked(coll.insert({a: 1}));
            assert.commandWorkedIgnoringWriteConcernErrors(
                db.runCommand({findAndModify: collName, query: {a: 1}, update: {b: 2}}));
        },
        confirmFunc: function(res) {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            assert.eq(res.lastErrorObject.updatedExisting, false);
            assert.eq(coll.find().itcount(), 1);
            assert.eq(coll.count({b: 2}), 1);
        }
    });

    // 'findAndModify' where the update has already been done.
    commands.push({
        req: {findAndModify: collName, query: {a: 1}, update: {$set: {b: 2}}},
        setupFunc: function() {
            assert.commandWorked(coll.insert({a: 1}));
            assert.commandWorkedIgnoringWriteConcernErrors(
                db.runCommand({findAndModify: collName, query: {a: 1}, update: {$set: {b: 2}}}));
        },
        confirmFunc: function(res) {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            assert.eq(res.lastErrorObject.updatedExisting, true);
            assert.eq(coll.find().itcount(), 1);
            assert.eq(coll.count({a: 1, b: 2}), 1);
        }
    });

    // 'findAndModify' where the document to delete does not exist.
    commands.push({
        req: {findAndModify: collName, query: {a: 1}, remove: true},
        setupFunc: function() {
            assert.commandWorked(coll.insert({a: 1}));
            assert.commandWorked(coll.remove({a: 1}));
        },
        confirmFunc: function(res) {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            assert.eq(res.lastErrorObject.n, 0);
        }
    });

    // 'dropDatabase' where the database has already been dropped.
    commands.push({
        req: {dropDatabase: 1},
        setupFunc: function() {
            assert.commandWorked(coll.insert({a: 1}));
            assert.commandWorkedIgnoringWriteConcernErrors(db.runCommand({dropDatabase: 1}));
        },
        confirmFunc: function(res) {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
        }
    });

    // 'drop' where the collection has already been dropped.
    commands.push({
        req: {drop: collName},
        setupFunc: function() {
            assert.commandWorked(coll.insert({a: 1}));
            assert.commandWorkedIgnoringWriteConcernErrors(db.runCommand({drop: collName}));
        },
        confirmFunc: function(res) {
            assert.commandFailedWithCode(res, ErrorCodes.NamespaceNotFound);
        }
    });

    // 'create' where the collection has already been created.
    commands.push({
        req: {create: collName},
        setupFunc: function() {
            assert.commandWorkedIgnoringWriteConcernErrors(db.runCommand({create: collName}));
        },
        confirmFunc: function(res) {
            assert.commandFailedWithCode(res, ErrorCodes.NamespaceExists);
        }
    });

    // 'insert' where the document with the same _id has already been inserted.
    commands.push({
        req: {insert: collName, documents: [{_id: 1}]},
        setupFunc: function() {
            assert.commandWorked(coll.insert({_id: 1}));
        },
        confirmFunc: function(res) {
            assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
            assert.eq(res.n, 0);
            assert.eq(res.writeErrors[0].code, ErrorCodes.DuplicateKey);
            assert.eq(coll.count({_id: 1}), 1);
        }
    });

    return commands;
}
