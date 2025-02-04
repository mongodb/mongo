
const t = db[jsTestName()];
t.drop();

assert.commandWorked(t.update({_id: 1, tags: {"$ne": "a"}}, {"$push": {tags: "a"}}, true));
assert.eq({_id: 1, tags: ["a"]}, t.findOne(), "A");

assert(t.drop());
