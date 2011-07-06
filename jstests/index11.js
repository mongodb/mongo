// Reindex w/ field too large to index

coll = db.jstests_index11;
coll.drop();

var str = "xxxxxxxxxxxxxxxx";
str = str + str;
str = str + str;
str = str + str;
str = str + str;
str = str + str;
str = str + str;
str = str + str;
str = str + str;
str = str + 'q';

coll.insert({ k: 'a', v: str });

assert.eq(0, coll.find({ "k": "x" }).count(), "expected zero keys 1");

coll.ensureIndex({"k": 1, "v": 1});
coll.insert({ k: "x", v: str });

assert.eq(0, coll.find({"k": "x"}).count(), "B"); // SERVER-1716

coll.dropIndexes();
coll.ensureIndex({"k": 1, "v": 1});

assert.eq(0, coll.find({ "k": "x" }).count(), "expected zero keys 2");
