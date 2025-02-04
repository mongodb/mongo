// @tags: [requires_non_retryable_writes, requires_fastcount]

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insert({_id: 1}));
assert.commandWorked(coll.insert({_id: 2}));
assert.commandWorked(coll.insert({_id: null}));

const obj = {
    foo: 1,
    nullElem: null
};

assert.writeErrorWithCode(coll.remove({x: obj.bar}), ErrorCodes.BadValue);
assert.eq(3, coll.count());

assert.writeErrorWithCode(coll.remove({x: undefined}), ErrorCodes.BadValue);
assert.eq(3, coll.count());

assert.writeErrorWithCode(coll.remove({_id: obj.bar}), ErrorCodes.BadValue);
assert.writeErrorWithCode(coll.remove({_id: undefined}), ErrorCodes.BadValue);

assert.commandWorked(coll.remove({_id: obj.nullElem}));
assert.eq(2, coll.count());

assert.commandWorked(coll.insert({_id: null}));
assert.eq(3, coll.count());

assert.writeErrorWithCode(coll.remove({_id: undefined}), ErrorCodes.BadValue);
assert.eq(3, coll.count());