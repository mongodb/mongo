t = db.text_blog;
t.drop();

t.save({_id: 1, title: "my blog post", text: "this is a new blog i am writing. yay"});
t.save({_id: 2, title: "my 2nd post", text: "this is a new blog i am writing. yay"});
t.save({_id: 3, title: "knives are Fun", text: "this is a new blog i am writing. yay"});

// default weight is 1
// specify weights if you want a field to be more meaningull
t.ensureIndex({"title": "text", text: "text"}, {weights: {title: 10}});

res = t.find({"$text": {"$search": "blog"}}, {score: {"$meta": "textScore"}}).sort({
    score: {"$meta": "textScore"}
});
assert.eq(3, res.length());
assert.eq(1, res[0]._id);

res = t.find({"$text": {"$search": "write"}}, {score: {"$meta": "textScore"}});
assert.eq(3, res.length());
assert.eq(res[0].score, res[1].score);
assert.eq(res[0].score, res[2].score);
