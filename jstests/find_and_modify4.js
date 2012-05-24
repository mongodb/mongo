t = db.find_and_modify4;
t.drop();

// this is the best way to build auto-increment
function getNextVal(counterName){
    var ret = t.findAndModify({
                query: {_id: counterName},
                update: {$inc: {val: 1}},
                upsert: true,
                'new': true,
                });
    return ret.val;
}

assert.eq(getNextVal("a"), 1);
assert.eq(getNextVal("a"), 2);
assert.eq(getNextVal("a"), 3);
assert.eq(getNextVal("z"), 1);
assert.eq(getNextVal("z"), 2);
assert.eq(getNextVal("a"), 4);

t.drop();

function helper(upsert){
    return t.findAndModify({
                query: {_id: "asdf"},
                update: {$inc: {val: 1}},
                upsert: upsert, 
                'new': false // the default
                });
}

// upsert:false so nothing there before and after
assert.eq(helper(false), null);
assert.eq(t.count(), 0);

// upsert:false so nothing there before; something there after
assert.eq(helper(true), null);
assert.eq(t.count(), 1);
assert.eq(helper(true),  {_id: 'asdf', val: 1});
assert.eq(helper(false), {_id: 'asdf', val: 2}); // upsert only matters when obj doesn't exist
assert.eq(helper(true),  {_id: 'asdf', val: 3});


// _id created if not specified
var out = t.findAndModify({
                query: {a:1},
                update: {$set: {b: 2}},
                upsert: true, 
                'new': true
                });
assert.neq(out._id, undefined);
assert.eq(out.a, 1);
assert.eq(out.b, 2);

