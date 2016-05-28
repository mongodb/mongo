
// test repl basics
// data on master/slave is the same

var rt = new ReplTest("basic1");

m = rt.start(true);
s = rt.start(false);

function block() {
    am.runCommand({getlasterror: 1, w: 2, wtimeout: 3000});
}

am = m.getDB("foo");
as = s.getDB("foo");

function check(note) {
    var start = new Date();
    var x, y;
    while ((new Date()).getTime() - start.getTime() < 30000) {
        x = am.runCommand("dbhash");
        y = as.runCommand("dbhash");
        if (x.md5 == y.md5)
            return;
        sleep(200);
    }
    lastOpLogEntry =
        m.getDB("local").oplog.$main.find({op: {$ne: "n"}}).sort({$natural: -1}).limit(-1).next();
    note = note + tojson(am.a.find().toArray()) + " != " + tojson(as.a.find().toArray()) +
        "last oplog:" + tojson(lastOpLogEntry);
    assert.eq(x.md5, y.md5, note);
}

am.a.save({x: 1});
check("A");

am.a.save({x: 5});

am.a.update({}, {$inc: {x: 1}});
check("B");

am.a.update({}, {$inc: {x: 1}}, false, true);
check("C");

// -----   check features -------

// map/reduce
assert.writeOK(am.mr.insert({tags: ["a"]}));
assert.writeOK(am.mr.insert({tags: ["a", "b"]}));
check("mr setup");

m = function() {
    for (var i = 0; i < this.tags.length; i++) {
        print("\t " + i);
        emit(this.tags[i], 1);
    }
};

r = function(key, v) {
    return Array.sum(v);
};

correct = {
    a: 2,
    b: 1
};

function checkMR(t) {
    var res = t.mapReduce(m, r, {out: {inline: 1}});
    assert.eq(correct, res.convertToSingleObject(), "checkMR: " + tojson(t));
}

function checkNumCollections(msg, diff) {
    if (!diff)
        diff = 0;
    var m = am.getCollectionNames();
    var s = as.getCollectionNames();
    assert.eq(m.length + diff, s.length, msg + " lengths bad \n" + tojson(m) + "\n" + tojson(s));
}

checkNumCollections("MR1");
checkMR(am.mr);
checkMR(as.mr);
checkNumCollections("MR2");

block();
checkNumCollections("MR3");

var res = am.mr.mapReduce(m, r, {out: "xyz"});
block();

checkNumCollections("MR4");

var t = am.rpos;
var writeOption = {writeConcern: {w: 2, wtimeout: 3000}};
t.insert({_id: 1, a: [{n: "a", c: 1}, {n: "b", c: 1}, {n: "c", c: 1}], b: [1, 2, 3]}, writeOption);
check("after pos 1 ");

t.update({"a.n": "b"}, {$inc: {"a.$.c": 1}}, writeOption);
check("after pos 2 ");

t.update({b: 2}, {$inc: {"b.$": 1}}, writeOption);
check("after pos 3 ");

t.update({b: 3}, {$set: {"b.$": 17}}, writeOption);
check("after pos 4 ");

printjson(am.rpos.findOne());
printjson(as.rpos.findOne());

// am.getSisterDB( "local" ).getCollection( "oplog.$main" ).find().limit(10).sort( { $natural : -1 }
// ).forEach( printjson )

t = am.b;
var updateOption = {upsert: true, multi: false, writeConcern: {w: 2, wtimeout: 3000}};
t.update({_id: "fun"}, {$inc: {"a.b.c.x": 6743}}, updateOption);
check("b 1");

t.update({_id: "fun"}, {$inc: {"a.b.c.x": 5}}, updateOption);
check("b 2");

t.update({_id: "fun"}, {$inc: {"a.b.c.x": 100, "a.b.c.y": 911}}, updateOption);
assert.eq({_id: "fun", a: {b: {c: {x: 6848, y: 911}}}}, as.b.findOne(), "b 3");
check("b 4");

// lots of indexes

am.lotOfIndexes.insert({x: 1});
for (i = 0; i < 200; i++) {
    var idx = {};
    idx["x" + i] = 1;
    am.lotOfIndexes.ensureIndex(idx);
}

assert.soon(function() {
    return am.lotOfIndexes.getIndexes().length == as.lotOfIndexes.getIndexes().length;
}, "lots of indexes a");

assert.eq(
    am.lotOfIndexes.getIndexes().length, as.lotOfIndexes.getIndexes().length, "lots of indexes b");

// multi-update with $inc

am.mu1.update({_id: 1, $atomic: 1}, {$inc: {x: 1}}, true, true);
x = {
    _id: 1,
    x: 1
};
assert.eq(x, am.mu1.findOne(), "mu1");
assert.soon(function() {
    z = as.mu1.findOne();
    printjson(z);
    return friendlyEqual(x, z);
}, "mu2");

// profiling - this should be last

am.setProfilingLevel(2);
am.foo.insert({x: 1}, writeOption);
am.foo.findOne();
assert.eq(2, am.system.profile.count(), "P1");
assert.eq(0, as.system.profile.count(), "P2");

assert.eq(1, as.foo.findOne().x, "P3");
assert.eq(0, as.system.profile.count(), "P4");

assert(as.getCollectionNames().indexOf("system.profile") < 0, "P4.5");

as.setProfilingLevel(2);
as.foo.findOne();
assert.eq(1, as.system.profile.count(), "P5");

rt.stop();
