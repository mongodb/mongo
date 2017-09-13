t = db.unset;
t.drop();

orig = {
    _id: 1,
    emb: {}
};
t.insert(orig);

t.update({_id: 1}, {$unset: {'emb.a': 1}});
t.update({_id: 1}, {$unset: {'z': 1}});
assert.eq(orig, t.findOne(), "A");

t.update({_id: 1}, {$set: {'emb.a': 1}});
t.update({_id: 1}, {$set: {'z': 1}});

t.update({_id: 1}, {$unset: {'emb.a': 1}});
t.update({_id: 1}, {$unset: {'z': 1}});
assert.eq(orig, t.findOne(), "B");  // note that emb isn't removed

t.update({_id: 1}, {$unset: {'emb': 1}});
assert.eq({_id: 1}, t.findOne(), "C");
