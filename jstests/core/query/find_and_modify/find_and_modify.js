// @tags: [
//   requires_fastcount,
// ]

const t = db[jsTestName()];
t.drop();

// fill db
for (let i = 1; i <= 10; i++) {
    assert.commandWorked(t.insert({priority: i, inprogress: false, value: 0}));
}

// returns old
let out = t.findAndModify({sort: {priority: 1}, update: {$set: {inprogress: true}, $inc: {value: 1}}});
assert.eq(out.value, 0);
assert.eq(out.inprogress, false);
assert.commandWorked(t.update({_id: out._id}, {$set: {inprogress: false}}));

// returns new
out = t.findAndModify({sort: {priority: 1}, update: {$set: {inprogress: true}, $inc: {value: 1}}, "new": true});
assert.eq(out.value, 2);
assert.eq(out.inprogress, true);
assert.commandWorked(t.update({_id: out._id}, {$set: {inprogress: false}}));

// update highest priority
out = t.findAndModify({query: {inprogress: false}, sort: {priority: -1}, update: {$set: {inprogress: true}}});
assert.eq(out.priority, 10);
// update next highest priority
out = t.findAndModify({query: {inprogress: false}, sort: {priority: -1}, update: {$set: {inprogress: true}}});
assert.eq(out.priority, 9);

// Use expressions in the 'fields' argument with 'new' false.
out = t.findAndModify({
    query: {inprogress: false},
    sort: {priority: -1},
    "new": false,
    update: {$set: {inprogress: true}, $inc: {value: 1}},
    fields: {priority: 1, inprogress: 1, computedField: {$add: ["$value", 2]}},
});
assert.eq(out.priority, 8);
assert.eq(out.inprogress, false);
// The projection should have been applied to the pre image of the update.
assert.eq(out.computedField, 2);

// Use expressions in the 'fields' argument with 'new' true.
out = t.findAndModify({
    query: {inprogress: false},
    sort: {priority: -1},
    update: {$set: {inprogress: true}, $inc: {value: 1}},
    "new": true,
    fields: {priority: 1, inprogress: 1, computedField: {$add: ["$value", 2]}},
});
assert.eq(out.priority, 7);
assert.eq(out.inprogress, true);
// The projection should have been applied to the update post image.
assert.eq(out.computedField, 3);

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
assert.commandWorked(t.insert({x: 1}));
assert.throws(function () {
    t.findAndModify({query: {x: 1}, update: {y: 2}, remove: true});
});
assert.throws(function () {
    t.findAndModify({query: {x: 1}, update: {y: 2}, remove: true, sort: {x: 1}});
});
assert.throws(function () {
    t.findAndModify({query: {x: 1}, update: {y: 2}, remove: true, upsert: true});
});
assert.throws(function () {
    t.findAndModify({query: {x: 1}, update: {y: 2}, new: true, remove: true});
});
assert.throws(function () {
    t.findAndModify({query: {x: 1}, upsert: true, remove: true});
});

//
// SERVER-17387: Find and modify should throw in the case of invalid projection.
//

function runFindAndModify(shouldMatch, upsert, newParam) {
    assert(t.drop());
    if (shouldMatch) {
        assert.commandWorked(t.insert({_id: "found"}));
    }
    const query = shouldMatch ? "found" : "miss";
    const res = db.runCommand({
        findAndModify: t.getName(),
        query: {_id: query},
        update: {$inc: {y: 1}},
        fields: {foo: {$pop: ["bar"]}},
        upsert: upsert,
        new: newParam,
    });
    assert.commandFailedWithCode(res, 31325);
}

// Insert case.
runFindAndModify(false /* shouldMatch */, true /* upsert */, true /* new */);

// Update with upsert + new.
runFindAndModify(true /* shouldMatch */, true /* upsert */, true /* new */);

// Update with just new: true.
runFindAndModify(true /* shouldMatch */, false /* upsert */, true /* new */);

// Update with just upsert: true.
runFindAndModify(true /* shouldMatch */, true /* upsert */, false /* new */);

// Update with neither upsert nor new flags.
runFindAndModify(true /* shouldMatch */, false /* upsert */, false /* new */);

//
// SERVER-17372
//

assert(t.drop());
let cmdRes = db.runCommand({findAndModify: t.getName(), query: {_id: "miss"}, update: {$inc: {y: 1}}, upsert: true});
assert.commandWorked(cmdRes);
assert("value" in cmdRes);
assert.eq(null, cmdRes.value);

cmdRes = db.runCommand({
    findAndModify: t.getName(),
    query: {_id: "missagain"},
    update: {$inc: {y: 1}},
    upsert: true,
    new: true,
});
assert.commandWorked(cmdRes);
assert("value" in cmdRes);
assert.eq("missagain", cmdRes.value._id);

// Two upserts should have happened.
assert.eq(2, t.count());
