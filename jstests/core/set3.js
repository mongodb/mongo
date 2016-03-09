
t = db.set3;
t.drop();

t.insert({"test1": {"test2": {"abcdefghijklmnopqrstu": {"id": 1}}}});
t.update({}, {"$set": {"test1.test2.abcdefghijklmnopqrstuvwxyz": {"id": 2}}});

x = t.findOne();
assert.eq(1, x.test1.test2.abcdefghijklmnopqrstu.id, "A");
assert.eq(2, x.test1.test2.abcdefghijklmnopqrstuvwxyz.id, "B");
