load("jstests/libs/fts.js");

t = db.text_parition1;
t.drop();

t.insert({_id: 1, x: 1, y: "foo"});
t.insert({_id: 2, x: 1, y: "bar"});
t.insert({_id: 3, x: 2, y: "foo"});
t.insert({_id: 4, x: 2, y: "bar"});

t.ensureIndex({x: 1, y: "text"});

assert.throws(function() {
    const cursor = t.find({"$text": {"$search": "foo"}});
    if (db.getMongo().readMode() === "legacy") {
        // In legacy read mode, calling next() will check if the response to the OP_QUERY message
        // has an error.
        cursor.next();
    } else {
        // In commands read mode, calling hasNext() will check if the find command returned an
        // error. We intentionally do not call next() to avoid masking errors caused by the cursor
        // exhausting all of its documents.
        cursor.hasNext();
    }
});

assert.eq([1], queryIDS(t, "foo", {x: 1}));

res = t.find({"$text": {"$search": "foo"}, x: 1}, {score: {"$meta": "textScore"}});
assert(res[0].score > 0, tojson(res.toArray()));

// repeat "search" with "language" specified, SERVER-8999
res = t.find({"$text": {"$search": "foo", "$language": "english"}, x: 1},
             {score: {"$meta": "textScore"}});
assert(res[0].score > 0, tojson(res.toArray()));
