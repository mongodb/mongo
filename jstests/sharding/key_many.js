import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

// Values have to be sorted - you must have exactly 6 values in each array
let types = [
    {name: "string", values: ["allan", "bob", "eliot", "joe", "mark", "sara"], keyfield: "k"},
    {name: "double", values: [1.2, 3.5, 4.5, 4.6, 6.7, 9.9], keyfield: "a"},
    {
        name: "date",
        values: [
            new Date(1000000),
            new Date(2000000),
            new Date(3000000),
            new Date(4000000),
            new Date(5000000),
            new Date(6000000),
        ],
        keyfield: "a",
    },
    {name: "string_id", values: ["allan", "bob", "eliot", "joe", "mark", "sara"], keyfield: "_id"},
    {name: "embedded 1", values: ["allan", "bob", "eliot", "joe", "mark", "sara"], keyfield: "a.b"},
    {
        name: "embedded 2",
        values: ["allan", "bob", "eliot", "joe", "mark", "sara"],
        keyfield: "a.b.c",
    },
    {
        name: "object",
        values: [
            {a: 1, b: 1.2},
            {a: 1, b: 3.5},
            {a: 1, b: 4.5},
            {a: 2, b: 1.2},
            {a: 2, b: 3.5},
            {a: 2, b: 4.5},
        ],
        keyfield: "o",
    },
    {
        name: "compound",
        values: [
            {a: 1, b: 1.2},
            {a: 1, b: 3.5},
            {a: 1, b: 4.5},
            {a: 2, b: 1.2},
            {a: 2, b: 3.5},
            {a: 2, b: 4.5},
        ],
        keyfield: "o",
        compound: true,
    },
    {
        name: "oid_id",
        values: [ObjectId(), ObjectId(), ObjectId(), ObjectId(), ObjectId(), ObjectId()],
        keyfield: "_id",
    },
    {
        name: "oid_other",
        values: [ObjectId(), ObjectId(), ObjectId(), ObjectId(), ObjectId(), ObjectId()],
        keyfield: "o",
    },
];

let s = new ShardingTest({name: "key_many", shards: 2});

assert.commandWorked(s.s0.adminCommand({enableSharding: "test", primaryShard: s.shard1.shardName}));

var db = s.getDB("test");
let primary = s.getPrimaryShard("test").getDB("test");
let secondary = s.getOther(primary).getDB("test");

let curT;

function makeObjectDotted(v) {
    let o = {};
    if (curT.compound) {
        let prefix = curT.keyfield + ".";
        if (typeof v == "object") {
            for (var key in v) o[prefix + key] = v[key];
        } else {
            for (var key in curT.values[0]) o[prefix + key] = v;
        }
    } else {
        o[curT.keyfield] = v;
    }
    return o;
}

function makeObject(v) {
    let o = {};
    let p = o;

    let keys = curT.keyfield.split(".");
    for (var i = 0; i < keys.length - 1; i++) {
        p[keys[i]] = {};
        p = p[keys[i]];
    }

    p[keys[i]] = v;

    return o;
}

function makeInQuery() {
    if (curT.compound) {
        // cheating a bit...
        return {"o.a": {$in: [1, 2]}};
    } else {
        return makeObjectDotted({$in: curT.values});
    }
}

function getKey(o) {
    let keys = curT.keyfield.split(".");
    for (let i = 0; i < keys.length; i++) {
        o = o[keys[i]];
    }
    return o;
}

Random.setRandomSeed();

for (let i = 0; i < types.length; i++) {
    curT = types[i];

    print("\n\n#### Now Testing " + curT.name + " ####\n\n");

    let shortName = "foo_" + curT.name;
    let longName = "test." + shortName;

    let c = db[shortName];
    s.adminCommand({shardcollection: longName, key: makeObjectDotted(1)});

    assert.eq(1, findChunksUtil.findChunksByNs(s.config, longName).count(), curT.name + " sanity check A");

    let unsorted = Array.shuffle(Object.extend([], curT.values));
    c.insert(makeObject(unsorted[0]));
    for (let x = 1; x < unsorted.length; x++) {
        c.save(makeObject(unsorted[x]));
    }

    assert.eq(6, c.find().count(), curT.name + " basic count");

    s.adminCommand({split: longName, middle: makeObjectDotted(curT.values[0])});
    s.adminCommand({split: longName, middle: makeObjectDotted(curT.values[2])});
    s.adminCommand({split: longName, middle: makeObjectDotted(curT.values[5])});

    s.adminCommand({
        movechunk: longName,
        find: makeObjectDotted(curT.values[2]),
        to: secondary.getMongo().name,
        _waitForDelete: true,
    });

    s.printChunks();

    assert.eq(3, primary[shortName].find().toArray().length, curT.name + " primary count");
    assert.eq(3, secondary[shortName].find().toArray().length, curT.name + " secondary count");

    assert.eq(6, c.find().toArray().length, curT.name + " total count");
    assert.eq(6, c.find().sort(makeObjectDotted(1)).toArray().length, curT.name + " total count sorted");

    assert.eq(6, c.find().sort(makeObjectDotted(1)).count(), curT.name + " total count with count()");

    assert.eq(
        2,
        c.find({$or: [makeObjectDotted(curT.values[2]), makeObjectDotted(curT.values[4])]}).count(),
        curT.name + " $or count()",
    );
    assert.eq(
        2,
        c
            .find({
                $or: [makeObjectDotted(curT.values[2]), makeObjectDotted(curT.values[4])],
            })
            .itcount(),
        curT.name + " $or itcount()",
    );
    assert.eq(
        4,
        c
            .find({
                $nor: [makeObjectDotted(curT.values[2]), makeObjectDotted(curT.values[4])],
            })
            .count(),
        curT.name + " $nor count()",
    );
    assert.eq(
        4,
        c
            .find({
                $nor: [makeObjectDotted(curT.values[2]), makeObjectDotted(curT.values[4])],
            })
            .itcount(),
        curT.name + " $nor itcount()",
    );

    let stats = c.stats();
    printjson(stats);
    assert.eq(6, stats.count, curT.name + " total count with stats()");

    let count = 0;
    for (let shard in stats.shards) {
        count += stats.shards[shard].count;
    }
    assert.eq(6, count, curT.name + " total count with stats() sum");

    assert.eq(curT.values, c.find().sort(makeObjectDotted(1)).toArray().map(getKey), curT.name + " sort 1");
    assert.eq(
        curT.values,
        c.find(makeInQuery()).sort(makeObjectDotted(1)).toArray().map(getKey),
        curT.name + " sort 1 - $in",
    );
    assert.eq(curT.values.reverse(), c.find().sort(makeObjectDotted(-1)).toArray().map(getKey), curT.name + " sort 2");

    assert.eq(0, c.find({xx: 17}).sort({zz: 1}).count(), curT.name + " xx 0a ");
    assert.eq(0, c.find({xx: 17}).sort(makeObjectDotted(1)).count(), curT.name + " xx 0b ");
    assert.eq(0, c.find({xx: 17}).count(), curT.name + " xx 0c ");
    assert.eq(0, c.find({xx: {$exists: true}}).count(), curT.name + " xx 1 ");

    c.update(makeObjectDotted(curT.values[3]), {$set: {xx: 17}});
    assert.eq(1, c.find({xx: {$exists: true}}).count(), curT.name + " xx 2 ");
    assert.eq(curT.values[3], getKey(c.findOne({xx: 17})), curT.name + " xx 3 ");

    assert.commandWorked(c.update(makeObjectDotted(curT.values[3]), {$set: {xx: 17}}, {upsert: true}));

    assert.commandWorked(c.createIndex({_id: 1}));

    // multi update
    var mysum = 0;
    c.find().forEach(function (z) {
        mysum += z.xx || 0;
    });
    assert.eq(17, mysum, curT.name + " multi update pre");

    c.update({}, {$inc: {xx: 1}}, false, true);

    var mysum = 0;
    c.find().forEach(function (z) {
        mysum += z.xx || 0;
    });
    assert.eq(23, mysum, curT.name + " multi update");
}

s.stop();
