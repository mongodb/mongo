/**
 * These tests verify that the oplog entries are created correctly for updates and that the updates
 * replicate properly from 2.6 to 2.6, 2.6 to 2.4, and (where applicable) 2.4 to 2.6.
 */
"use strict";
load( './jstests/multiVersion/libs/multi_rs.js' );

var testUpdates = function (primary, secondary) {
var testUpdate = function (obj) {
    // dont run modifiers that dont exist in 2.4 when 2.4 is primary
    if (primary === "2.4" && obj.two_four_incompatible) {
        return;
    }

    // initialize testing state
    var asserts_saved = obj.asserts_saved || false;
    var multi = obj.multi || false;
    var docs = obj.docs || [];
    var query = obj.query || {};
    var doUpsert = obj.doUpsert || false;

    var msg = obj.msg + " with a " + primary + " primary and a " + secondary + " secondary ";
    var oplog = obj.oplog || [];
    // use the 2.4 oplog entries if the primary is 2.4
    if (primary === "2.4" && obj.two_four_oplog) {
        oplog = obj.two_four_oplog;
    }
    var results = obj.results || [];
    var update = obj.update;

    if (!update) {
        assert(false, "invalid testUpdate object: no update object");
    }

    // insert initial date and make sure oplog looks proper
    coll.remove({});
    assert.eq(coll.count(), 0, "collection not empty")
    for (i = 0; i < docs.length; i++) {
        coll.save(docs[i]);
    }
    replTest.awaitReplication();
    if (docs.length) {
        assertLastOplog(docs, null, "save " + msg);
    }

    // run update and check results
    coll.update(query, update, {upsert: doUpsert, multi: multi});
    var gle = cdb.getLastErrorObj();
    // ensure update worked
    printjson(gle);
    assert.isnull(gle.err, "update failed for " + msg + ": " + tojson(gle));
    assert.eq(gle.n, results.length, "update failed for '" + msg +"': "+ tojson(gle));

    // special case where we pass in a function, needed for $currentDate
    if (typeof(results[0]) === 'function') {
        // run function with document and latest oplog entry as args
        results[0](coll.find({_id:1})[0],
            master.getDB("local").oplog.rs.find().limit(1).sort({$natural:-1}).next());
        // assert the secondary has the same thing as us
        assert.docEq(coll.find({_id:1})[0], secondaryColl.find({_id:1})[0]);
        return;
    }

    // normal case for upsert
    if (doUpsert) {
        assert(gle.upserted, "not upserted");
        results[0]["_id"] = gle.upserted;
        if (oplog.length) {
            oplog[0]["_id"] = gle.upserted;
        }
        assertLastOplog(oplog, null, msg);
    }
    // normal case for non-upsert
    else {
        assertLastOplog(oplog, results, msg);
    }
}


var assertLastOplog = function(o, o2 , msg) {
    // for each object in the collection
    for (i = o.length-1; i >= 0; i--) {
        // verify oplog entry looks as expected
        var last = oplog_coll.find().skip(o.length-i-1).limit(1).sort({$natural:-1}).next();
        printjson(last);
        assert.eq(last.ns, coll.getFullName(), "ns bad : " + msg);
        assert.docEq(last.o, o[i], "o bad : " + msg);
        if (o2) {
            assert.docEq(last.o2, {_id: o2[i]["_id"]}, "o2 bad : " + msg);
        }
        // make sure both nodes have the doc as we expect it to look
        var primary_doc = coll.find({_id:i+1})[0];
        var secondary_doc = secondaryColl.find({_id:i+1})[0];
        if (o2) {
            assert.docEq(primary_doc, o2[i]);
            // this will need to change if there are multi updates that only effect some documents
            var collDropEntry = oplog_coll.find().skip(o.length * 2).sort({$natural: -1}).next();
            assert.eq(collDropEntry.ns, "oplog_entries_from_updates.fake");
            assert.eq(collDropEntry.op, "d");
        }
        assert.docEq(primary_doc, secondary_doc);
    }
}

// set up replset
var name = "oplog_entries_from_updates";

var nodes = { n1 : { binVersion : primary },
              n2 : { binVersion : secondary } };
var replTest = new ReplSetTest({name: name,
                                nodes: nodes,
                                oplogSize:2,
                                nodeOptions: {smallfiles:""}});
nodes = replTest.startSet();
replTest.initiate();
var master = replTest.getMaster();
var config = master.getDB("local").system.replset.findOne();
config.version++;
config.settings = {getLastErrorDefaults: {w:2}};
var result = master.adminCommand({replSetReconfig : config});
assert.eq(result.ok, 1);

var coll = master.getDB(name).fake;
var secondaryColl = replTest.liveNodes.slaves[0].getDB(name).fake;
var cdb = coll.getDB();
var oplog_coll = master.getDB("local").oplog.rs
// set things up.
coll.drop();
coll.save({_id:1});
assertLastOplog({_id:1}, null, "save -- setup ");

/**
 * The first ones are from the old updatetests which tested the internal impl using a modSet
 *
 * Example testUpdate(), fields contain their default values:
 * testUpdate({
 *     msg: "", // a short description of the test that will appear in error messages
 *     docs: [], // array of objects to insert prior to running the update
 *     update: {}, // the object containing update operations
 *     results: [], // array containing the resulting documents expected from the update
 *     oplog: [], // array containing the oplog entries expected from the updates
 *     doUpsert: false, // whether or not the update should be an upsert
 *     two_four_oplog: null, // array of oplog entries to be used with a 2.4 primary (if it differs)
 *     two_four_incompatible: false, // can mark the test as invalid in 2.4 (uses new operators)
 *     multi: false, // whether or not the update should be a multiupdate
 *     query: {}, // the query to find the documents to update
 * });
 */

testUpdate({
    asserts_saved: true,
    msg: "IncRewriteExistingField: $inc $set",
    docs: [{_id:1, a:2}],
    update: {$inc:{a:1}, $set:{b:2}},
    results: [{_id:1, a:3, b:2}],
    oplog: [{$set:{a:3, b:2}}],
});

testUpdate({
    asserts_saved: true,
    msg: "IncRewriteNonExistingField: $inc $set",
    docs: [{_id:1, c:0}],
    update: {$inc:{a:1}, $set:{b:2}},
    results: [{_id:1, c:0, a:1, b:2}],
    oplog: [{$set:{a:1, b:2}}],
});

testUpdate({
    asserts_saved: true,
    msg: "TwoNestedPulls: two $pull",
    docs: [{_id:1, a:{ b:[ 1, 2 ], c:[ 1, 2 ] }}],
    update: {$pull:{ 'a.b':2, 'a.c':2 }},
    results: [{_id:1, a:{ b:[ 1 ], c:[ 1 ] }}],
    oplog: [{$set:{'a.b':[1], 'a.c':[1]}}],
});

testUpdate({
    asserts_saved: true,
    msg: "MultiSets: two $set",
    docs: [{_id:1, a:1, b:1}],
    update: {$set: {a:2, b:2}},
    results: [{_id:1, a:2, b:2}],
    oplog: [{$set:{a:2, b:2}}],
});

// More tests to validate the oplog format and correct excution

testUpdate({
    asserts_saved: true,
    msg: "single $set",
    docs: [{_id:1, a:1}],
    update: {$set:{a:2}},
    results: [{_id:1, a:2}],
    oplog: [{$set:{a:2}}],
});

testUpdate({
    msg: "single $inc",
    docs: [{_id:1, a:2}],
    update: {$inc:{a:1}},
    results: [{_id:1, a:3}],
    oplog: [{$set:{a:3}}],
});

testUpdate({
    msg: "$set no op",
    docs: [{_id:1, a:3}],
    update: {$set:{a:3}},
    results: [{_id:1, a:3}],
});

testUpdate({
    msg: "upsert that just inserts",
    update: {_id:ObjectId("52dfe31fc7db7feec4c6c485"), a:3},
    doUpsert: true,
    results: [{_id:ObjectId("52dfe31fc7db7feec4c6c485"), a:3}],
});

testUpdate({
    msg: "double $set",
    docs: [{_id:1, a:3}],
    update: {$set:{a:2, b:2}},
    results: [{_id:1, a:2, b:2}],
    oplog: [{$set:{a:2, b:2}}],
});

testUpdate({
    msg: "array $inc",
    docs: [{_id:1, a:[2]}],
    update: {$inc:{"a.0":1}},
    results: [{_id:1, a:[3]}],
    oplog: [{$set:{"a.0": 3}}],
});

testUpdate({
    msg: "$setOnInsert",
    docs: [{_id:1, a:[3]}],
    update: {$setOnInsert:{"a":-1}},
    results: [{_id:1, a:[3]}],
});

testUpdate({
    msg: "$setOnInsert w/upsert",
    update: {$setOnInsert:{"a":200}},
    doUpsert: true,
    results: [{a:200}],
    oplog: [{"a": 200}],
});

testUpdate({
    msg: "array $push 2",
    docs: [{_id:1, a:"foo"}],
    update: {$push:{c:18}},
    results: [{_id:1, a:"foo", c:[18]}],
    oplog: [{$set:{"c": [18]}}],
});

testUpdate({
    msg: "array $push $slice",
    docs: [{_id:1, a:{b:[18]}}],
    update: {$push:{"a.b":{$each:[1,2], $slice:-2}}},
    query: {_id:{$gt:0}},
    results: [{_id:1, a: {b:[1,2]}}],
    oplog: [{$set:{"a.b": [1,2]}}],
});

testUpdate({
    msg: "array $push $sort ($slice -100)",
    docs: [{_id:1, a:{b:[{c:2}, {c:1}]}}],
    update: {$push:{"a.b":{$each:[{c:-1}], $sort:{"c":1}, $slice:-100}}},
    results: [{_id:1, a: {b:[{c:-1}, {c:1}, {c:2}]}}],
    oplog: [{$set:{"a.b": [{c:-1},{c:1}, {c:2}]}}],
});

testUpdate({
    msg: "array $push $slice $sort",
    docs: [{_id:1, a:[{b:2}, {b:1}]}],
    update: {$push:{"a":{$each:[{b:-1}], $slice:-2, $sort:{b:1}}}},
    query: {_id:{$gt:0}},
    results: [{_id:1, a: [{b:1}, {b:2}]}],
    oplog: [{$set:{a: [{b:1},{b:2}]}}],
});

testUpdate({
    msg: "array $push $slice $sort first two",
    docs: [{_id:1, a:{b:[{c:2}, {c:1}]}}],
    update: {$push:{"a.b":{$each:[{c:-1}], $slice:-2, $sort:{"c":1}}}},
    query: {_id:{$gt:0}},
    results: [{_id:1, a: {b:[{c:1}, {c:2}]}}],
    oplog: [{$set:{"a.b": [{c:1},{c:2}]}}],
});

testUpdate({
    msg: "array $push $slice $sort reversed first two",
    docs: [{_id:1, a:{b:[{c:1}, {c:2}]}}],
    update: {$push:{"a.b":{$each:[{c:-1}], $slice:-2, $sort:{"c":-1}}}},
    query: {_id:{$gt:0}},
    results: [{_id:1, a: {b:[{c:1}, {c:-1}]}}],
    oplog: [{$set:{"a.b": [{c:1},{c:-1}]}}],
});

testUpdate({
    asserts_saved: true,
    msg: "$rename",
    docs: [{_id:1, a:1}],
    update: {$rename:{'a':'b'}},
    query: {_id: {$gt:0}},
    results: [{_id:1, b:1}],
    oplog: [{$set:{"b": 1}, $unset: {"a": true}}],
    two_four_oplog: [{$set:{"b": 1}, $unset: {"a": 1}}],
});

testUpdate({
    asserts_saved: true,
    msg: "$unset",
    docs: [{_id:1, a:1, b:10}],
    update: {$unset:{'a':''}},
    results: [{_id:1, b:10}],
    oplog: [{$unset: {"a": true}}],
    two_four_oplog: [{$unset: {"a": 1}}],
});

testUpdate({
    asserts_saved: true,
    msg: "$xor",
    two_four_incompatible: true,
    docs: [{_id:1, a: NumberInt(3)}],
    update: {$bit: {a: {xor: NumberInt(5)}}},
    results: [{_id:1, a:6}],
    oplog: [{$set:{"a": 6}}],
});

testUpdate({
    asserts_saved: true,
    msg: "$or",
    docs: [{_id:1, a:NumberInt(2)}],
    update: {$bit: {a: {or: NumberInt(7)}}},
    results: [{_id:1, a:7}],
    oplog: [{$set:{"a": 7}}],
});

testUpdate({
    asserts_saved: true,
    msg: "$and",
    docs: [{_id:1, a:NumberInt(2)}],
    update: {$bit: {a: {and: NumberInt(8)}}},
    results: [{_id:1, a:0}],
    oplog: [{$set:{"a": 0}}],
});

testUpdate({
    asserts_saved: true,
    msg: "$currentDate",
    two_four_incompatible: true,
    docs: [{_id:1, a:2}],
    update: {$currentDate: {a: true}},
    results: [function(doc, oplog) {}],
    oplog: [{$set:{"a": 2}}],
});

testUpdate({
    asserts_saved: true,
    msg: "$currentDate and $type date",
    two_four_incompatible: true,
    docs: [{_id:1, a:2}],
    update: {$currentDate: {a: {$type: "date"}}},
    results: [function(doc, oplog) {}],
    oplog: [{$set:{"a": 2}}],
});

testUpdate({
    asserts_saved: true,
    msg: "$currentDate and $type timestamp",
    two_four_incompatible: true,
    docs: [{_id:1, a:2}],
    update: {$currentDate: {a: {$type: "timestamp"}}},
    results: [function(doc, oplog) {}],
    oplog: [{$set:{"a": 2}}],
});

testUpdate({
    msg: "$min",
    two_four_incompatible: true,
    docs: [{_id:1, a:2}],
    update: {$min: {a: 1}},
    results: [{_id:1, a:1}],
    oplog: [{$set:{"a": 1}}],
});

testUpdate({
    asserts_saved: true,
    msg: "$max",
    two_four_incompatible: true,
    docs: [{_id:1, a:2}],
    update: {$max: {a: 7}},
    results: [{_id:1, a:7}],
    oplog: [{$set:{"a": 7}}],
});

testUpdate({
    asserts_saved: true,
    msg: "$pullAll",
    docs: [{_id:1, a:[1,3,4,9,2,12]}],
    update: {$pullAll: {a: [12,4,8]}},
    results: [{_id:1, a:[1,3,9,2]}],
    oplog: [{$set:{"a": [1,3,9,2]}}],
});

testUpdate({
    asserts_saved: true,
    msg: "$pushAll",
    docs: [{_id:1, a:[1,3,4,9,2]}],
    update: {$pushAll: {a: [12,4,8]}},
    results: [{_id:1, a:[1,3,4,9,2,12,4,8]}],
    oplog: [{$set:{"a.5": 12, 'a.6': 4, 'a.7': 8}}],
    two_four_oplog: [{$set:{"a": [1, 3, 4, 9, 2, 12, 4, 8]}}],
});

testUpdate({
    asserts_saved: true,
    msg: "$pop",
    docs: [{_id:1, a:[1,3,4,9,2]}],
    update: {$pop: {a: 1}},
    results: [{_id:1, a:[1,3,4,9]}],
    oplog: [{$set: {a: [1,3,4,9]}}],
});

testUpdate({
    asserts_saved: true,
    msg: "$addToSet",
    docs: [{_id:1, a:[1,3,4,9,2]}],
    update: {$addToSet: {a: {$each: [12,4,8]}}},
    results: [{_id:1, a:[1,3,4,9,2,12,8]}],
    oplog: [{$set:{a:[1,3,4,9,2,12,8]}}],
});

testUpdate({
    asserts_saved: true,
    msg: "$position",
    two_four_incompatible: true,
    docs: [{_id:1, a:[1,3,4,9,2]}],
    update: {$push: {a: {$each: [12], $position: 3}}},
    results: [{_id:1, a:[1,3,4,12,9,2]}],
    oplog: [{$set:{"a": [1,3,4,12,9,2]}}],
});

testUpdate({
    asserts_saved: true,
    msg: "$ (array position $)",
    docs: [{_id:1, a:[1,3,4,9,2]}],
    update: {$inc: {'a.$': 1}},
    query: {_id:1, a:9},
    results: [{_id:1, a:[1,3,4,10,2]}],
    oplog: [{$set:{"a.3": 10}}],
});

testUpdate({
    asserts_saved: true,
    msg: "$mul",
    two_four_incompatible: true,
    docs: [{_id:1, a:2}],
    update: {$mul: {a: -17}},
    results: [{_id:1, a:-34}],
    oplog: [{$set:{"a": -34}}],
});

testUpdate({
    asserts_saved: true,
    msg: "nested arrays",
    two_four_incompatible: true,
    docs: [{_id:1, a:[2,4,5,[1,45,6]]}],
    update: {$mul: {"a.3.1": -2}},
    results: [{_id:1, a:[2,4,5,[1,-90,6]]}],
    oplog: [{$set:{"a.3.1": -90}}],
});

testUpdate({
    asserts_saved: true,
    msg: "very nested arrays, multiple modifiers",
    two_four_incompatible: true,
    docs: [{_id:1, a:[18,1,3,2,[1,46,[2,4,5,[1,45,6],7,5,[12,3],3],129,12],1,7,3]}],
    update: {$mul: {"a.4.2.3.1": -2}, $inc: {'a.4.2.6.1': 1}},
    results: [{_id:1, a:[18,1,3,2,[1,46,[2,4,5,[1,-90,6],7,5,[12,4],3],129,12],1,7,3]}],
    oplog: [{$set:{"a.4.2.3.1": -90, 'a.4.2.6.1': 4}}],
});


testUpdate({
    asserts_saved: true,
    msg: "very nested documents, multiple modifiers",
    two_four_incompatible: true,
    docs: [{_id:1, a: {b: {c: 2, d: 7}, e: {f: {g: 15, h: 9}}}}],
    update: {$mul: {"a.b.c": -9}, $inc: {"a.e.f.g": 7}},
    results: [{_id:1, a: {b: {c: -18, d: 7}, e: {f: {g: 22, h: 9}}}}],
    oplog: [{$set: {"a.b.c": -18, "a.e.f.g": 22}}],
});

testUpdate({
    asserts_saved: true,
    multi: true,
    msg: "multi $rename",
    docs: [{_id:1, a:1},
                    {_id:2, a: [11,4,6]},
                    {_id:3, a: "hello"}],
    update: {$rename:{'a':'b'}},
    query: {_id: {$gt:0}},
    results: [{_id:1, b:1},
              {_id:2, b: [11,4,6]},
              {_id:3, b: "hello"}],
    oplog: [{$set:{"b": 1}, $unset: {"a": true}},
            {$set:{"b": [11,4,6]}, $unset: {"a": true}},
            {$set:{"b": "hello"}, $unset: {"a": true}}],
    two_four_oplog: [{$set:{"b": 1}, $unset: {"a": 1}},
                     {$set:{"b": [11,4,6]}, $unset: {"a": 1}},
                     {$set:{"b": "hello"}, $unset: {"a": 1}}],
});

testUpdate({
    asserts_saved: true,
    multi: true,
    msg: "multi $set and $inc",
    docs: [{_id:1, a:1},
                    {_id:2, a:73},
                    {_id:3, a:26}],
    update: {$set:{'b':'hello'}, $inc:{'a':1}},
    query: {_id: {$gt:0}},
    results: [{_id:1, a:2, b: "hello"},
              {_id:2, a:74, b: "hello"},
              {_id:3, a:27, b: "hello"}],
    oplog: [{$set:{"b": "hello", "a": 2}},
            {$set:{"b": "hello", "a": 74}},
            {$set:{"b": "hello", "a": 27}}],
});

testUpdate({
    asserts_saved: true,
    multi: true,
    msg: "multi $push",
    docs: [{_id:1, a:[1,6,3]},
                    {_id:2, a:[31, "monkeys"]},
                    {_id:3, a:["hi", "yo"]}],
    update: {$push:{'a':'hello'}},
    query: {_id: {$gt:0}},
    results: [{_id:1, a:[1,6,3, "hello"]},
              {_id:2, a:[31, "monkeys", "hello"]},
              {_id:3, a:["hi", "yo", "hello"]}],
    oplog: [{$set:{"a.3": "hello"}},
            {$set:{"a.2": "hello"}},
            {$set:{"a.2": "hello"}}],
});

testUpdate({
    asserts_saved: true,
    two_four_incompatible: true,
    multi: true,
    msg: "multi $max and $mul",
    docs: [{_id:1, a:1, b: 31},
                    {_id:2, a:73, b:481},
                    {_id:3, a:26, b:974}],
    update: {$max:{'b':456}, $mul:{'a':3}},
    query: {_id: {$gt:0}},
    results: [{_id:1, a:3, b: 456},
              {_id:2, a:219, b: 481},
              {_id:3, a:78, b:974}],
    oplog: [{$set: {a:3, b: 456}},
            {$set: {a:219}},
            {$set: {a:78}}],
});

testUpdate({
    asserts_saved: true,
    two_four_incompatible: true,
    multi: true,
    msg: "multi $min and $push",
    docs: [{_id:1, a:1, b: [3,1]},
                    {_id:2, a:73, b:[4,8,1]},
                    {_id:3, a:26, b:[9,7,4]}],
    update: {$push:{'b':4}, $min:{'a':3}},
    query: {_id: {$gt:0}},
    results: [{_id:1, a:1, b: [3,1,4]},
              {_id:2, a:3, b:[4,8,1,4]},
              {_id:3, a:3, b:[9,7,4,4]}],
    oplog: [{$set: {"b.2": 4}},
            {$set: {a:3, "b.3": 4}},
            {$set: {a:3, "b.3":4}}],
});

testUpdate({
    asserts_saved: true,
    multi: true,
    msg: "multi $inc and $pull",
    docs: [{_id:1, a:1, b: [3,1]},
                    {_id:2, a:73, b:[4,8,1]},
                    {_id:3, a:26, b:[9,7,4]}],
    update: {$pull:{'b':4}, $inc:{'a':3}},
    query: {_id: {$gt:-1}},
    results: [{_id:1, a:4, b: [3,1]},
              {_id:2, a:76, b:[8,1]},
              {_id:3, a:29, b:[9,7]}],
    oplog: [{$set: {a: 4}},
            {$set: {a:76, "b": [8,1]}},
            {$set: {a:29, "b":[9,7]}}],
    two_four_oplog: [{$set: {a: 4, b: [3,1]}},
                     {$set: {a:76, "b": [8,1]}},
                     {$set: {a:29, "b":[9,7]}}],
});

replTest.stopSet();
}

testUpdates("2.6", "2.4");
testUpdates("2.4", "2.6");
testUpdates("2.4", "2.4");
