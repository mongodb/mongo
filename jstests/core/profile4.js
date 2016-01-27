/**
 * Check debug information recorded for a query.
 */

function profileCursor() {
    var userStr = username + "@" + db.getName();
    return db.system.profile.find({user: userStr});
}

function getLastOp() {
    return profileCursor().sort({$natural: -1}).next();
}

// Special db so that it can be run in parallel tests. Also need to create a user so that
// operations run by this test (and not other tests running in parallel) can be identified.
var stddb = db;
var db = db.getSisterDB("profile4");

db.dropAllUsers();
var coll = db.profile4;
coll.drop();

var username = "jstests_profile4_user";
db.createUser({user: username, pwd: "password", roles: jsTest.basicUserRoles});
db.auth(username, "password");

try {
    var lastOp;

    // Clear the profiling collection.
    db.setProfilingLevel(0);
    db.system.profile.drop();
    assert.eq(0 , profileCursor().count());

    // Enable profiling. It will be disabled again at the end of the test, or if the test fails.
    db.setProfilingLevel(2);

    coll.find().itcount();
    lastOp = getLastOp();
    assert.eq(lastOp.op, "query");
    assert.eq(lastOp.query.find, coll.getName());
    assert.eq(lastOp.ns, coll.getFullName());
    assert.eq(lastOp.keysExamined, 0);
    assert.eq(lastOp.keyUpdates, 0);
    assert.eq(lastOp.nreturned, 0);
    assert.eq(lastOp.cursorExhausted, true);

    // Check write lock stats are set.
    coll.save({});
    lastOp = getLastOp();
    assert.eq(lastOp.op, "insert");
    assert.lt(0, Object.keys(lastOp.locks).length);

    // Check read lock stats are set.
    coll.find();
    lastOp = getLastOp();
    assert.eq(lastOp.op, "query");
    assert.lt(0, Object.keys(lastOp.locks).length);

    coll.save({});
    coll.save({});
    coll.find().skip(1).limit(4).itcount();
    lastOp = getLastOp();
    assert.eq(lastOp.query.skip, 1)
    assert.eq(lastOp.docsExamined, 3);
    assert.eq(lastOp.nreturned, 2);
    // Find command will use "limit", OP_QUERY will use ntoreturn.
    var expectedField = db.getMongo().useReadCommands() ? "limit" : "ntoreturn";
    assert.eq(lastOp.query[expectedField], 4);

    coll.find().batchSize(2).next();
    lastOp = getLastOp();
    assert.lt(0, lastOp.cursorid);

    coll.find({a: 1}).itcount();
    lastOp = getLastOp();
    assert.eq(lastOp.query.filter, {a: 1});

    coll.find({_id: 0}).itcount();
    lastOp = getLastOp();
    assert.eq(lastOp.idhack, true);

    coll.find().sort({a: 1}).itcount();
    lastOp = getLastOp();
    assert.eq(lastOp.hasSortStage, true);

    coll.ensureIndex({a: 1});
    coll.find({a: 1}).itcount();
    lastOp = getLastOp();
    assert.eq("FETCH", lastOp.execStats.stage, tojson(lastOp.execStats));
    assert.eq("IXSCAN", lastOp.execStats.inputStage.stage, tojson(lastOp.execStats));

    // For queries with a lot of stats data, the execution stats in the profile is replaced by
    // the plan summary.
    var orClauses = 32;
    var bigOrQuery = { $or: [] };
    for (var i = 0; i < orClauses; ++i) {
        var indexSpec = {};
        indexSpec["a" + i] = 1;
        coll.ensureIndex(indexSpec);
        bigOrQuery["$or"].push(indexSpec);
    }
    coll.find(bigOrQuery).itcount();
    lastOp = getLastOp();
    assert.neq(undefined, lastOp.execStats.summary, tojson(lastOp.execStats));

    // Confirm "cursorExhausted" not set when cursor is open.
    coll.drop();
    coll.insert([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}]);
    coll.find().batchSize(2).next(); // Query performed leaving open cursor
    lastOp = getLastOp();
    assert.eq(lastOp.op, "query");
    assert(!("cursorExhausted" in lastOp));

    var cursor = coll.find().batchSize(2);
    cursor.next(); // Perform initial query and consume first of 2 docs returned.
    cursor.next(); // Consume second of 2 docs from initial query.
    cursor.next(); // getMore performed, leaving open cursor.
    lastOp = getLastOp();
    assert.eq(lastOp.op, "getmore");
    assert(!("cursorExhausted" in lastOp));

    // Exhaust cursor and confirm getMore has "cursorExhausted:true".
    cursor.itcount();
    lastOp = getLastOp();
    assert.eq(lastOp.cursorExhausted, true);

    // OP_QUERY-specific test for the "wrapped" query predicate form.
    if (!db.getMongo().useReadCommands()) {
        // Must accept non-dollar-prefixed forms of "query" and "orderby".
        coll.find({query: {_id: {$gte: 0}}, orderby: {_id: 1}}).itcount();
        lastOp = getLastOp();
        assert.eq(lastOp.op, "query");
        assert.eq(lastOp.query.find, coll.getName());
        assert.eq(lastOp.query.filter, {_id: {$gte: 0}});
        assert.eq(lastOp.query.sort, {_id: 1});

        // Should also work with dollar-prefixed forms.
        coll.find({$query: {_id: {$gte: 0}}, $orderby: {_id: 1}}).itcount();
        lastOp = getLastOp();
        assert.eq(lastOp.op, "query");
        assert.eq(lastOp.query.find, coll.getName());
        assert.eq(lastOp.query.filter, {_id: {$gte: 0}});
        assert.eq(lastOp.query.sort, {_id: 1});

        // Negative ntoreturn.
        coll.find().limit(-3).itcount();
        lastOp = getLastOp();
        assert.eq(lastOp.query.ntoreturn, -3);
    }

    // getMore command should show up as a getMore operation, not a command.
    var cmdRes = db.runCommand({find: coll.getName(), batchSize: 1});
    assert.commandWorked(cmdRes);
    db.runCommand({getMore: cmdRes.cursor.id, collection: coll.getName()});
    lastOp = getLastOp();
    assert.eq(lastOp.op, "getmore");
    assert.eq(lastOp.ns, coll.getFullName());

    // getMore entry created by iterating the cursor should have the same format, regardless of
    // readMode.
    coll.find().batchSize(3).itcount();
    lastOp = getLastOp();
    assert.eq(lastOp.op, "getmore");
    assert.eq(lastOp.ns, coll.getFullName());
    assert("getMore" in lastOp.query);
    assert.eq(lastOp.query.getMore, lastOp.cursorid);
    assert.eq(lastOp.query.collection, coll.getName());
    assert.eq(lastOp.query.batchSize, 3)
    assert.eq(lastOp.cursorExhausted, true)
    assert.eq(lastOp.nreturned, 2);
    assert("responseLength" in lastOp);

    // Ensure that special $-prefixed OP_QUERY options like $hint and $returnKey get added to the
    // profiler entry correctly.
    coll.find().hint({_id: 1}).itcount();
    lastOp = getLastOp();
    assert.eq(lastOp.query.hint, {_id: 1});

    coll.find().comment("a comment").itcount();
    lastOp = getLastOp();
    assert.eq(lastOp.query.comment, "a comment");

    coll.find().maxScan(3000).itcount();
    lastOp = getLastOp();
    assert.eq(lastOp.query.maxScan, 3000);

    coll.find().maxTimeMS(4000).itcount();
    lastOp = getLastOp();
    assert.eq(lastOp.query.maxTimeMS, 4000);

    coll.find().max({_id: 3}).itcount();
    lastOp = getLastOp();
    assert.eq(lastOp.query.max, {_id: 3});

    coll.find().min({_id: 0}).itcount();
    lastOp = getLastOp();
    assert.eq(lastOp.query.min, {_id: 0});

    coll.find().returnKey().itcount();
    lastOp = getLastOp();
    assert.eq(lastOp.query.returnKey, true);

    coll.find().snapshot().itcount();
    lastOp = getLastOp();
    assert.eq(lastOp.query.snapshot, true);

    // Tests for profiling findAndModify.
    coll.drop();
    for (var i = 0; i < 3; i++) {
        assert.writeOK(coll.insert({_id: i, a: i}));
    }

    // Update as findAndModify.
    assert.eq({_id: 2, a: 2}, coll.findAndModify({query: {a: 2}, update: {$inc: {b: 1}}}));
    lastOp = getLastOp();
    assert.eq(lastOp.op, "command");
    assert.eq(lastOp.ns, coll.getFullName());
    assert.eq(lastOp.command.query, {a: 2});
    assert.eq(lastOp.command.update, {$inc: {b: 1}});
    assert.eq(lastOp.updateobj, {$inc: {b: 1}});
    assert.eq(lastOp.keysExamined, 0);
    assert.eq(lastOp.docsExamined, 3);
    assert.eq(lastOp.nMatched, 1);
    assert.eq(lastOp.nModified, 1);

    // Delete as findAndModify.
    assert.eq({_id: 2, a: 2, b: 1}, coll.findAndModify({query: {a: 2}, remove: true}));
    lastOp = getLastOp();
    assert.eq(lastOp.op, "command");
    assert.eq(lastOp.ns, coll.getFullName());
    assert.eq(lastOp.command.query, {a: 2});
    assert.eq(lastOp.command.remove, true);
    assert(!("updateobj" in lastOp));
    assert.eq(lastOp.ndeleted, 1);

    // Update with {upsert: true} as findAndModify.
    assert.eq({_id: 2, a: 2, b: 1}, coll.findAndModify({
        query: {_id: 2, a: 2},
        update: {$inc: {b: 1}},
        upsert: true,
        new: true
    }));
    lastOp = getLastOp();
    assert.eq(lastOp.op, "command");
    assert.eq(lastOp.ns, coll.getFullName());
    assert.eq(lastOp.command.query, {_id: 2, a: 2});
    assert.eq(lastOp.command.update, {$inc: {b: 1}});
    assert.eq(lastOp.command.upsert, true);
    assert.eq(lastOp.command.new, true);
    assert.eq(lastOp.updateobj, {$inc: {b: 1}});
    assert.eq(lastOp.keysExamined, 0);
    assert.eq(lastOp.docsExamined, 0);
    assert.eq(lastOp.nMatched, 0);
    assert.eq(lastOp.nModified, 0);
    assert.eq(lastOp.upsert, true);

    // Idhack update as findAndModify.
    assert.eq({_id: 2, a: 2, b: 1}, coll.findAndModify({
        query: {_id: 2},
        update: {$inc: {b: 1}}
    }));
    lastOp = getLastOp();
    assert.eq(lastOp.keysExamined, 1);
    assert.eq(lastOp.docsExamined, 1);
    assert.eq(lastOp.nMatched, 1);
    assert.eq(lastOp.nModified, 1);

    // Update as findAndModify with projection.
    assert.eq({a: 2}, coll.findAndModify({
        query: {a: 2},
        update: {$inc: {b: 1}},
        fields: {_id: 0, a: 1}
    }));
    lastOp = getLastOp();
    assert.eq(lastOp.op, "command");
    assert.eq(lastOp.ns, coll.getFullName());
    assert.eq(lastOp.command.query, {a: 2});
    assert.eq(lastOp.command.update, {$inc: {b: 1}});
    assert.eq(lastOp.command.fields, {_id: 0, a: 1});
    assert.eq(lastOp.updateobj, {$inc: {b: 1}});
    assert.eq(lastOp.keysExamined, 0);
    assert.eq(lastOp.docsExamined, 3);
    assert.eq(lastOp.nMatched, 1);
    assert.eq(lastOp.nModified, 1);

    // Delete as findAndModify with projection.
    assert.eq({a: 2}, coll.findAndModify({
        query: {a: 2},
        remove: true,
        fields: {_id: 0, a: 1}
    }));
    lastOp = getLastOp();
    assert.eq(lastOp.op, "command");
    assert.eq(lastOp.ns, coll.getFullName());
    assert.eq(lastOp.command.query, {a: 2});
    assert.eq(lastOp.command.remove, true);
    assert.eq(lastOp.command.fields, {_id: 0, a: 1});
    assert(!("updateobj" in lastOp));
    assert.eq(lastOp.ndeleted, 1);

    // Tests for profiling update
    coll.drop();
    for (var i = 0; i < 3; i++) {
        assert.writeOK(coll.insert({_id: i, a: i}));
    }

    // Update
    coll.update({a: 2}, {$inc: {b: 1}});
    lastOp = getLastOp();
    assert.eq(lastOp.op, "update");    assert.eq(lastOp.ns, coll.getFullName());
    assert.eq(lastOp.query, {a: 2});
    assert.eq(lastOp.updateobj, {$inc: {b: 1}});
    assert.eq(lastOp.keysExamined, 0);
    assert.eq(lastOp.docsExamined, 3);
    assert.eq(lastOp.nMatched, 1);
    assert.eq(lastOp.nModified, 1);

    // Update with {upsert: true}
    coll.update({_id: 4, a: 4}, {$inc: {b: 1}}, {upsert: true});
    lastOp = getLastOp();
    assert.eq(lastOp.op, "update");
    assert.eq(lastOp.ns, coll.getFullName());
    assert.eq(lastOp.query, {_id: 4, a: 4});
    assert.eq(lastOp.updateobj, {$inc: {b: 1}});
    assert.eq(lastOp.keysExamined, 0);
    assert.eq(lastOp.docsExamined, 0);
    assert.eq(lastOp.nMatched, 0);
    assert.eq(lastOp.nModified, 0);
    assert.eq(lastOp.upsert, true);

    db.setProfilingLevel(0);
    db.system.profile.drop();
}
finally {
    db.setProfilingLevel(0);
    db = stddb;
}
