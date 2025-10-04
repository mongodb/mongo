// Cannot implicitly shard accessed collections because of not being able to create unique index
// using hashed shard key pattern.
// @tags: [cannot_create_unique_index_when_using_hashed_shard_key, requires_fastcount]

// unique index test for a case where the object grows
// and must move

// see indexa.js for the test case for an update with dup id check
// when it doesn't move

let t = db.indexb;
t.drop();
t.createIndex({a: 1}, true);

t.insert({a: 1});

let x = {a: 2};
t.save(x);

{
    assert(t.count() == 2, "count wrong B");

    x.a = 1;
    x.filler = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    t.save(x); // should fail, not unique.

    assert(t.count() == 2, "count wrong");
    assert(t.find({a: 1}).count() == 1, "bfail1");
    assert(t.find({a: 2}).count() == 1, "bfail2");
}
