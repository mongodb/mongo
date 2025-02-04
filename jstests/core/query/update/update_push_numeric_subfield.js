
const t = db[jsTestName()];
t.drop();

const orig = {
    "_id": 1,
    "question": "a",
    "choices": {"1": {"choice": "b"}, "0": {"choice": "c"}},

};

assert.commandWorked(t.save(orig));
assert.eq(orig, t.findOne(), "A");

assert.commandWorked(
    t.update({_id: 1, 'choices.0.votes': {$ne: 1}}, {$push: {'choices.0.votes': 1}}));

orig.choices["0"].votes = [1];
assert.eq(orig.choices["0"], t.findOne().choices["0"], "B");
