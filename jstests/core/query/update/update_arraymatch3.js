// @tags: [requires_multi_updates, requires_non_retryable_writes]

const t = db[jsTestName()];
t.drop();

let o = {_id: 1, title: "ABC", comments: [{"by": "joe", "votes": 3}, {"by": "jane", "votes": 7}]};

assert.commandWorked(t.save(o));
assert.eq(o, t.findOne(), "A1");

assert.commandWorked(
    t.update({'comments.by': 'joe'}, {$inc: {'comments.$.votes': 1}}, false, true));
o.comments[0].votes++;
assert.eq(o, t.findOne(), "A2");
