t = db.find_and_modify;
t.drop();

// fill db
for(var i=1; i<=10; i++) {
    t.insert({priority:i, inprogress:false, value:0});
}

// returns old
out = t.findAndModify({update: {$set: {inprogress: true}, $inc: {value:1}}});
assert.eq(out.value, 0);
assert.eq(out.inprogress, false);
t.update({_id: out._id}, {$set: {inprogress: false}});

// returns new
out = t.findAndModify({update: {$set: {inprogress: true}, $inc: {value:1}}, 'new': true});
assert.eq(out.value, 2);
assert.eq(out.inprogress, true);
t.update({_id: out._id}, {$set: {inprogress: false}});

// update highest priority
out = t.findAndModify({query: {inprogress:false}, sort:{priority:-1}, update: {$set: {inprogress: true}}});
assert.eq(out.priority, 10);
// update next highest priority
out = t.findAndModify({query: {inprogress:false}, sort:{priority:-1}, update: {$set: {inprogress: true}}});
assert.eq(out.priority, 9);

// remove lowest priority
out = t.findAndModify({sort:{priority:1}, remove:true});
assert.eq(out.priority, 1);

// remove next lowest priority
out = t.findAndModify({sort:{priority:1}, remove:1});
assert.eq(out.priority, 2);

// return null (was {} before 1.5.4) if no matches (drivers may handle this differently)
out = t.findAndModify({query:{no_such_field:1}, remove:1});
assert.eq(out, null);
