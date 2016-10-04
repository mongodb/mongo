t = db.find_and_modify;
t.drop();

// fill db
for (var i = 1; i <= 10; i++) {
    t.insert({priority: i, inprogress: false, value: 0});
}

// returns old
out = t.findAndModify({update: {$set: {inprogress: true}, $inc: {value: 1}}});
assert.eq(out.value, 0);
assert.eq(out.inprogress, false);
t.update({_id: out._id}, {$set: {inprogress: false}});

// returns new
out = t.findAndModify({update: {$set: {inprogress: true}, $inc: {value: 1}}, 'new': true});
assert.eq(out.value, 2);
assert.eq(out.inprogress, true);
t.update({_id: out._id}, {$set: {inprogress: false}});

// update highest priority
out = t.findAndModify(
    {query: {inprogress: false}, sort: {priority: -1}, update: {$set: {inprogress: true}}});
assert.eq(out.priority, 10);
// update next highest priority
out = t.findAndModify(
    {query: {inprogress: false}, sort: {priority: -1}, update: {$set: {inprogress: true}}});
assert.eq(out.priority, 9);

// remove lowest priority
out = t.findAndModify({sort: {priority: 1}, remove: true});
assert.eq(out.priority, 1);

// remove next lowest priority
out = t.findAndModify({sort: {priority: 1}, remove: 1});
assert.eq(out.priority, 2);

// return null (was {} before 1.5.4) if no matches (drivers may handle this differently)
out = t.findAndModify({query: {no_such_field: 1}, remove: 1});
assert.eq(out, null);

// make sure we fail with conflicting params to findAndModify SERVER-16601
t.insert({x: 1});
assert.throws(function() {
    t.findAndModify({query: {x: 1}, update: {y: 2}, remove: true});
});
assert.throws(function() {
    t.findAndModify({query: {x: 1}, update: {y: 2}, remove: true, sort: {x: 1}});
});
assert.throws(function() {
    t.findAndModify({query: {x: 1}, update: {y: 2}, remove: true, upsert: true});
});
assert.throws(function() {
    t.findAndModify({query: {x: 1}, update: {y: 2}, new: true, remove: true});
});
assert.throws(function() {
    t.findAndModify({query: {x: 1}, upsert: true, remove: true});
});

//
// SERVER-17387: Find and modify should throw in the case of invalid projection.
//

t.drop();

// Insert case.
var cmdRes = db.runCommand({
    findAndModify: t.getName(),
    query: {_id: "miss"},
    update: {$inc: {y: 1}},
    fields: {foo: {$pop: ["bar"]}},
    upsert: true,
    new: true
});
assert.commandFailed(cmdRes);

t.insert({_id: "found"});

// Update with upsert + new.
cmdRes = db.runCommand({
    findAndModify: t.getName(),
    query: {_id: "found"},
    update: {$inc: {y: 1}},
    fields: {foo: {$pop: ["bar"]}},
    upsert: true,
    new: true
});
assert.commandFailed(cmdRes);

// Update with just new: true.
cmdRes = db.runCommand({
    findAndModify: t.getName(),
    query: {_id: "found"},
    update: {$inc: {y: 1}},
    fields: {foo: {$pop: ["bar"]}},
    new: true
});
assert.commandFailed(cmdRes);

// Update with just upsert: true.
cmdRes = db.runCommand({
    findAndModify: t.getName(),
    query: {_id: "found"},
    update: {$inc: {y: 1}},
    fields: {foo: {$pop: ["bar"]}},
    upsert: true
});
assert.commandFailed(cmdRes);

// Update with neither upsert nor new flags.
cmdRes = db.runCommand({
    findAndModify: t.getName(),
    query: {_id: "found"},
    update: {$inc: {y: 1}},
    fields: {foo: {$pop: ["bar"]}},
});
assert.commandFailed(cmdRes);

//
// SERVER-17372
//

t.drop();
cmdRes = db.runCommand(
    {findAndModify: t.getName(), query: {_id: "miss"}, update: {$inc: {y: 1}}, upsert: true});
assert.commandWorked(cmdRes);
assert("value" in cmdRes);
assert.eq(null, cmdRes.value);

cmdRes = db.runCommand({
    findAndModify: t.getName(),
    query: {_id: "missagain"},
    update: {$inc: {y: 1}},
    upsert: true,
    new: true
});
assert.commandWorked(cmdRes);
assert("value" in cmdRes);
assert.eq("missagain", cmdRes.value._id);

// Two upserts should have happened.
assert.eq(2, t.count());
