t = db.text_blogwild;
t.drop();

t.save({_id: 1, title: "my blog post", text: "this is a new blog i am writing. yay eliot"});
t.save({_id: 2, title: "my 2nd post", text: "this is a new blog i am writing. yay"});
t.save({
    _id: 3,
    title: "knives are Fun for writing eliot",
    text: "this is a new blog i am writing. yay"
});

// default weight is 1
// specify weights if you want a field to be more meaningull
t.ensureIndex({dummy: "text"}, {weights: "$**"});

res = t.find({"$text": {"$search": "blog"}});
assert.eq(3, res.length(), "A1");

res = t.find({"$text": {"$search": "write"}});
assert.eq(3, res.length(), "B1");

// mixing
t.dropIndex("dummy_text");
assert.eq(1, t.getIndexKeys().length, "C1");
t.ensureIndex({dummy: "text"}, {weights: {"$**": 1, title: 2}});

res = t.find({"$text": {"$search": "write"}}, {score: {"$meta": "textScore"}}).sort({
    score: {"$meta": "textScore"}
});
assert.eq(3, res.length(), "C2");
assert.eq(3, res[0]._id, "C3");

res = t.find({"$text": {"$search": "blog"}}, {score: {"$meta": "textScore"}}).sort({
    score: {"$meta": "textScore"}
});
assert.eq(3, res.length(), "D1");
assert.eq(1, res[0]._id, "D2");

res = t.find({"$text": {"$search": "eliot"}}, {score: {"$meta": "textScore"}}).sort({
    score: {"$meta": "textScore"}
});
assert.eq(2, res.length(), "E1");
assert.eq(3, res[0]._id, "E2");
