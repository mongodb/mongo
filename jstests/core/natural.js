// SERVER-14580: Test $natural

// Iterate cursor and check that field is increasing or decreasing
//   name - Test name
//   cur - cursor
//   field - field name to check
//   dir - direction for test (-1 for decreasing or 1 for increasing)
//   shardTest - true/false
//   errorExpected - expected error true/false
function checkQuery(name, cur, field, dir, shardTest, errorExpected) {
    var minVal = -1;
    var maxVal = Number.MAX_VALUE;
    var lastVal = minVal;
    var lastDiskLoc = {file: minVal, offset: minVal, initOffset: minVal};

    if (dir === -1) {
        lastVal = maxVal;
        lastDiskLoc = {file: maxVal, offset: maxVal, initOffset: maxVal};
    }
    try {
        while(cur.hasNext()) {
            // Field is an increasing or decreasing value
            var val = cur.next();
            delete(val.pad);
            // Only test diskLoc if specified from projection in query
            var testdl = false;
            if ("diskLoc" in val) {
                testdl = true;
                // Reset last offset if file changes
                if (val.diskLoc.file != lastDiskLoc.file) {
                    lastDiskLoc.offset = lastDiskLoc.initOffset;
                }
            }
            if (dir === 1) {
                assert.gte(val[field], lastVal, name+" "+field);
                // diskLoc tests are informational, not assertable
                if (testdl) {
                    if (val.diskLoc.file < lastDiskLoc.file) {
                        print("Failure: "+name+" diskLoc file: "+
                              val.diskLoc.file+" < "+lastDiskLoc.file);
                        printjson(val);
                    }
                    if (val.diskLoc.offset <= lastDiskLoc.offset) {
                        print("Failure: "+name+" diskLoc file: "+
                              val.diskLoc.offset+" <= "+lastDiskLoc.offset);
                        printjson(val);
                    }
                }
            } else {
                assert.lte(val[field], lastVal, name+" "+field);
                // diskLoc tests are informational, not assertable
                if (testdl) {
                    if (val.diskLoc.file > lastDiskLoc.file) {
                        print("Failure: "+name+" diskLoc file: "+
                              val.diskLoc.file+" > "+lastDiskLoc.file);
                        printjson(val);
                    }
                    if (val.diskLoc.offset >= lastDiskLoc.offset) {
                        print("Failure: "+name+" diskLoc file: "+
                              val.diskLoc.offset+" >= "+lastDiskLoc.offset);
                        printjson(val);
                    }
                }
            }
            lastVal = val[field];
            lastDiskLoc = val.diskLoc;
        }
    }
    // Check if error is expected
    catch(err) {
        // Rethrow if no error is expected
        if (!errorExpected) {
            // shardTest should swallow assert, as docs do not have a preserved order
            if (!shardTest) {
                throw(err);
            }
        }
    }
}

// Run test cases
//   t - test object
//   conn - connection to DB
//   shardTest - if sharding test, true/false
var runTests = function(t, conn, shardTest) {

    var name = t.name;
    if (shardTest) {
        name += " sharding";
        // Not a shard test if no shardKey specified
        if (!("shardKey" in t)) {
            return;
        }
    }
    // print test name and test info
    jsTest.log(name);
    printjson(t);

    var collOptions = {};
    if ("collOptions" in t) {
        collOptions = t.collOptions;
    }
    var dbName = "natural";

    // Optional test attributes
    // Test throws error
    var err = false;
    if ("errorExpected" in t) {
        err = t.errorExpected;
    }

    assert.commandWorked(conn.getDB(dbName).createCollection(name, collOptions));
    var coll = conn.getDB(dbName)[name];

    // Set indexes on collection
    var sharded = false;
    t.indexes.forEach(function(idx) {
        // Test has second arg to ensureIndex
        var idxOpt = null;
        if ("indexOpt" in idx) {
            idxOpt = idx.indexOpt;
        }
        coll.ensureIndex(idx.index, idxOpt);
        if (shardTest && !sharded) {
            sharded = true;
            conn.getDB("admin").runCommand
                ({shardCollection : coll+"", key: t.shardKey});
            // pre-split collection on shardKey
            var splitCmd = {split: coll+"", middle: {}};
            for (var k in t.shardKey) {
                splitCmd.middle[k] = docNum/2;
            }
            conn.getDB("admin").runCommand(splitCmd);
        }
    });

    // Insert docs
    t.inserts(coll);

    printjson(coll.getDB().stats());

    // Note: $meta not supported before 2.6
    var dl = {diskLoc: {$meta: "diskloc"}};

    var projs = [];
    if ("projections" in t) {
        projs = t.projections;
    }
    var order = t.orderField;
    t.queries.forEach(function(q) {
        // Tests with hint should override sort $natural
        if ("hints" in t) {
            t.hints.forEach(function(hint) {
                checkQuery(name+" hint override",
                    coll.find(q).sort({$natural: -1}).hint(hint), order, 1, shardTest, err);
            });
        }
        // Check all queries with sort & hint $natural in both directions: -1 & 1
        // Project the diskLoc
        else {
            [-1, 1].forEach(function(dir) {
                checkQuery(name+" sort "+dir,
                    coll.find(q, dl).sort({$natural: dir}),
                    order, dir, shardTest, err);
                checkQuery(name+" hint "+dir,
                    coll.find(q, dl).hint({$natural: dir}),
                    order, dir, shardTest, err);
                checkQuery(name+" sort/hint "+dir,
                    coll.find(q, dl).sort({$natural: dir}).hint({$natural: dir}),
                    order, dir, shardTest, err);
                checkQuery(name+" sort/hint opposite",
                    coll.find(q, dl).sort({$natural: dir}).hint({$natural: dir*-1}),
                    order, dir, shardTest, err);
                // Test various projections
                projs.forEach(function(proj) {
                    checkQuery(name+" proj sort "+dir,
                        coll.find(q, proj).sort({$natural: dir}),
                        order, dir, shardTest, err);
                    checkQuery(name+" proj hint "+dir,
                        coll.find(q, proj).hint({$natural: dir}),
                        order, dir, shardTest, err);
                    checkQuery(name+" proj sort/hint "+dir,
                        coll.find(q, proj).sort({$natural: dir}).hint({$natural: dir}),
                        order, dir, shardTest, err);
                    checkQuery(name+" proj sort/hint opposite",
                        coll.find(q, proj).sort({$natural: dir}).hint({$natural: dir*-1}),
                        order, dir, shardTest, err);
                });
            });
        }
    });

    // Drop collection
    if (shardTest) {
        conn.stopBalancer();
    }
    assert(function() {return coll.drop()}, "Coll drop"+coll);
    if (shardTest) {
        conn.startBalancer();
    }
}

// Test cases, each test is a doc
//   name - Test name
//   indexes - array of index docs
//   shardKey - shard key to use for sharding test
//   inserts - function to insert docs under test into collection
//   queries - array of query docs to execute on collection
//   projections - array of projections to apply to query
//   orderField - field name which is inserted in order
//   hints  - array of hint docs
//   errorExpected - if test is expected to fail (default false is unspecified)

var docNum = 1000;
var padSize = Math.pow(2,16);
var padding = "";
for (var i=0; i< padSize; i++) {
    padding += "a";
}
tests = [
    {name: "Regular index",
     indexes: [{index: {a: 1}}],
     shardKey: {a: 1},
     inserts: function(c) {for (var i=0; i< docNum; i++)
               {c.insert({a: Math.floor(Math.random()*docNum), b: i%7, c: i, pad: padding})}},
     queries: [{},
               {a: {$gt: 80}},
               {a: {$lt: 30}}],
     orderField: "c"
    },
    {name: "Regular index - covered query",
     indexes: [{index: {a: 1}}],
     shardKey: {a: 1},
     inserts: function(c) {for (var i=0; i< docNum; i++) {
                 c.insert({a: Math.floor(Math.random()*docNum), b: i%7, c: i, pad: padding})}},
     queries: [{},
               {a: {$gt: 80}}, 
               {a: {$lt: 30}}],
     projections: [{a: 1, c: 1, _id: 0}, {c: 1, _id: 0}],
     orderField: "c"
    },
    {name: "Hint index",
     indexes: [{index: {a: 1}}, {index: {b: 1}}],
     shardKey: {a: 1},
     inserts: function(c) {for (var i=0; i< docNum; i++) {
                c.insert({a: Math.floor(Math.random()*docNum*10), b: i%7, c: i, pad: padding})}},
     queries: [{},
               {a: {$gt: 90000}}, 
               {a: {$lt: 20000}}],
     orderField: "a",
     hints: [{a: 1}, "a_1"]
    },
    {name: "Regular index duplicate",
     indexes: [{index: {b: 1}}],
     shardKey: {a: 1},
     inserts: function(c) {for (var i=0; i< docNum; i++) {
                c.insert({a: Math.floor(Math.random()*docNum), b: i%7, c: i, pad: padding})}},
     queries: [{b: {$lt: 4}}, 
               {b: {$gt: 5}}],
     orderField: "c"
    },
    {name: "Compound index",
     indexes: [{index: {a: 1, b: 1}}],
     shardKey: {a: 1, b: 1},
     inserts: function(c) {for (var i=0; i< docNum; i++) {
                c.insert({a: Math.floor(Math.random()*docNum), b: i%7, c: i, pad: padding})}},
     queries: [{a: {$gt: 20}, b: {$lt: 4}},
               {a: {$gt: 20}, b: {$gt: 5}}],
     orderField: "c"
    },
    {name: "Unique index",
     indexes: [{index: {a: 1, unique: true}}],
     shardKey: {a: 1},
     inserts: function(c) {for (var i=0; i< docNum; i++) {
                c.insert({a: Math.floor(Math.random()*docNum), b: i%7, c: i, pad: padding})}},
     queries: [{a: {$gt: 80}},
               {a: {$gt: 30}}],
     orderField: "c"
    },
    {name: "Geo 2d index",
     indexes: [{index: {b: "2d"}}],
     shardKey: {b: 1},
     inserts: function(c) {for (var i=0; i< docNum; i++) {
                c.insert({a: Math.floor(Math.random()*docNum),
                          b: {loc: [i%90, 45-i%45]},
                          c: i,
                          pad: padding})}},
     queries: [{b: {$near: [1, 2]}}],
     orderField: "c",
     errorExpected: true
    },
    {name: "Geo 2dsphere index",
     indexes: [{index: {b: "2dsphere"}}],
     shardKey: {b: 1},
     inserts: function(c) {for (var i=0; i< docNum; i++) {
                c.insert({a: Math.floor(Math.random()*docNum),
                          b: {type: "Point", coordinates: [i%90, 45-i%45]},
                          c: i,
                          pad: padding})}},
     queries: [{b: {$near: {$geometry: {type: "Point", coordinates: [1, 2]}}}}],
     orderField: "c",
     errorExpected: true
    },
    {name: "Geo geoHaystack index",
     indexes: [{index: {b: "geoHaystack"}}],
     shardKey: {b: 1},
     inserts: function(c) {for (var i=0; i< docNum; i++) {
                c.insert({a: Math.floor(Math.random()*docNum),
                          b: {type: "Point", coordinates: [i%90, 45-i%45]},
                          c: i})}},
     queries: [{b: {$near: {$geometry: {type: "Point", coordinates: [1, 2]}}}}],
     orderField: "c",
     errorExpected: true
    },
    {name: "Text index",
     indexes: [{index: {b: "text"}}],
     shardKey: {b: 1},
     inserts: function(c) {var words = ["bird", "cat", "dog", "goat"];
                           for (var i=0; i< docNum; i++) {
                              c.insert({a: Math.floor(Math.random()*docNum),
                                        b: words[i%4],
                                        c: i,
                                        pad: padding})}},
     queries: [{$text: {$search: "cat"}},
               {b: "cat"}],
     orderField: "c",
     errorExpected: true
    },
    {name: "Sparse index",
     indexes: [{index: {b: 1}, indexOpt: {sparse: true}}],
     shardKey: {c: 1},
     inserts: function(c) {for (var i=0; i< docNum; i++) {
                c.insert({a: Math.floor(Math.random()*docNum), b: i%7, c: i, pad: padding})}},
     queries: [{b: {$gt: 3}}],
     orderField: "c"
    },
    {name: "Hashed index",
     indexes: [{index: {b: "hashed"}}],
     shardKey: {b: 1},
     inserts: function(c) {for (var i=0; i< docNum; i++) {
                c.insert({a: Math.floor(Math.random()*docNum), b: i%7, c: i, pad: padding})}},
     queries: [{b: {$lt: 4}},
               {b: {$gt: 5}}],
     orderField: "c"
    },
    {name: "TTL index",
     // expire is set to 5 years
     indexes: [{index: {a: 1}, index2: {expireAfterSeconds: 60*60*24*365*5}}],
     shardKey: {a: 1},
     inserts: function(c) {var myDate = new Date();
                           for (var i=0; i< docNum; i++) {
                              // Year is set to a random value from 2000-2019
                              myDate.setYear(Math.floor(Math.random()*docNum%20+2000));
                              c.insert({a: myDate, b: i%7, c: i, pad: padding})}},
     queries: [{a: {$lt: ISODate("2012-01-01T00:00:00Z")}},
               {a: {$gt: ISODate("2015-01-01T00:00:00Z")}}],
     orderField: "c"
    },
    {name: "Capped",
     // cap collection at 75% of docNum
     collOptions: {capped: true,
                   size: (padSize+100)*(docNum-(docNum*.25)),
                   max: docNum-(docNum*.25)},
     indexes: [{index: {a: 1}}],
     inserts: function(c) {for (var i=0; i< docNum; i++)
               {c.insert({a: Math.floor(Math.random()*docNum), b: i%7, c: i, pad: padding})}},
     queries: [{},
               {a: {$gt: 80}},
               {a: {$lt: 30}}],
     orderField: "c"
    }
]


// Main program
var dbName = "natural";

// Standalone tests
var conn = MongoRunner.runMongod();
for (var i=0; i<tests.length; i++) {
    runTests(tests[i], conn, false);
}
MongoRunner.stopMongod(conn.port);

// Sharding tests
var cluster = {name: "natural", shards: 2, mongos: 1, config: 1,
               other: {shardOptions: {smallfiles: ""}}};
var st = new ShardingTest(cluster);
var mongos = st.s;
assert.commandWorked(mongos.getDB("admin").runCommand({enablesharding : dbName}),
    "Enable sharding on "+dbName);

for (var i=0; i<tests.length; i++) {
    runTests(tests[i], st, true);
}
st.stop();
