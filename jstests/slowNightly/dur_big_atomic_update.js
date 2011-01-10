// @file dur_big_atomic_update.js
//
// this tests writing 1GB in an atomic update to make sure we commit periodically

var path = "/data/db/dur_big_atomic_update";

conn = startMongodEmpty("--port", 30001, "--dbpath", path, "--dur", "--durOptions", 8);
d = conn.getDB("test");
d.foo.drop();

for (var i=0; i<1024; i++){
    d.foo.insert({_id:i});
}

big_string = 'x';
while (big_string.length < 1024*1024) {
    big_string += big_string;
}

d.foo.update({$atomic:1}, {$set: {big_string: big_string}}, false, /*multi*/true);
err = d.getLastErrorObj();

assert(err.err == null);
assert(err.n == 1024);

// free up space
d.dropDatabase();

stopMongod(30001);

print("dur big atomic update SUCCESS");
