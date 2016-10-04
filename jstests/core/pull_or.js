
t = db.pull_or;
t.drop();

doc = {
    _id: 1,
    a: {b: [{x: 1}, {y: 'y'}, {x: 2}, {z: 'z'}]}
};

t.insert(doc);

t.update({}, {$pull: {'a.b': {'y': {$exists: true}}}});

assert.eq([{x: 1}, {x: 2}, {z: 'z'}], t.findOne().a.b);

t.drop();
t.insert(doc);
t.update({}, {$pull: {'a.b': {$or: [{'y': {$exists: true}}, {'z': {$exists: true}}]}}});

assert.eq([{x: 1}, {x: 2}], t.findOne().a.b);
