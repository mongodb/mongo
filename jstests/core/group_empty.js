
t = db.group_empty;
t.drop();

res1 = db.runCommand(
    {group: {$reduce: function() {}, ns: 'group_empty', cond: {}, key: {}, initial: {count: 0}}});
t.ensureIndex({x: 1});
res2 = db.runCommand(
    {group: {$reduce: function() {}, ns: 'group_empty', cond: {}, key: {}, initial: {count: 0}}});

assert.docEq(res1.retval, res2.retval);
assert.eq(res1.keys, res2.keys);
assert.eq(res1.count, res2.count);
