
t = db.text_phrase;
t.drop();

t.save({_id: 1, title: "my blog post", text: "i am writing a blog. yay"});
t.save({_id: 2, title: "my 2nd post", text: "this is a new blog i am typing. yay"});
t.save({_id: 3, title: "knives are Fun", text: "this is a new blog i am writing. yay"});

t.ensureIndex({"title": "text", text: "text"}, {weights: {title: 10}});

res = t.find({"$text": {"$search": "blog write"}}, {score: {"$meta": "textScore"}}).sort({
    score: {"$meta": "textScore"}
});
assert.eq(3, res.length());
assert.eq(1, res[0]._id);
assert(res[0].score > (res[1].score * 2), tojson(res.toArray()));

res = t.find({"$text": {"$search": "write blog"}}, {score: {"$meta": "textScore"}}).sort({
    score: {"$meta": "textScore"}
});
assert.eq(3, res.length());
assert.eq(1, res[0]._id);
assert(res[0].score > (res[1].score * 2), tojson(res.toArray()));
