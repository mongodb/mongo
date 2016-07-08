// SERVER-7720 Building an index with a too-long name should always fail
// Formerly, we would allow an index that already existed to be "created" with too long a name,
// but this caused secondaries to crash when replicating what should be a bad createIndex command.
// Here we test that the too-long name is rejected in this situation as well

t = db.long_index_rename;
t.drop();

for (i = 1; i < 10; i++) {
    t.save({a: i});
}

t.createIndex({a: 1}, {name: "aaa"});
var result = t.createIndex({a: 1}, {
    name: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" +
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
});
assert(!result.ok);
