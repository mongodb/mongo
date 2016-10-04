// @file dur_big_atomic_update.js
//
// this tests writing 1GB in an atomic update to make sure we commit periodically

var conn = MongoRunner.runMongod({journal: "", journalOptions: 8});
d = conn.getDB("test");
d.foo.drop();

var bulk = d.foo.initializeUnorderedBulkOp();
for (var i = 0; i < 1024; i++) {
    bulk.insert({_id: i});
}
assert.writeOK(bulk.execute());

var server_bits = db.serverStatus().mem.bits;
var big_string_size = (server_bits == 32 ? 64 * 1024 : 1024 * 1024);

var big_string = 'xxxxxxxxxxxxxxxx';
while (big_string.length < big_string_size) {
    big_string += big_string;
}

var res = assert.writeOK(
    d.foo.update({$atomic: 1}, {$set: {big_string: big_string}}, false, true /* multi */));
assert.eq(1024, res.nModified);

d.dropDatabase();

bulk = d.foo.initializeUnorderedBulkOp();
for (var i = 0; i < 1024; i++) {
    bulk.insert({_id: i});
}
assert.writeOK(bulk.execute());

// Do it again but in a db.eval
d.eval(function(big_string) {
    new Mongo().getDB("test").foo.update(
        {}, {$set: {big_string: big_string}}, false, /*multi*/ true);
}, big_string);  // Can't pass in connection or DB objects

err = d.getLastErrorObj();

assert(err.err == null);
assert(err.n == 1024);

// free up space
d.dropDatabase();

MongoRunner.stopMongod(conn);

print("dur big atomic update SUCCESS");
