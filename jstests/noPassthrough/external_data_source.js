/**
 * Tests basic syntax check of $_externalDataSources aggregate command option.
 */
(function() {
"use strict";

// Runs tests on a standalone mongod.
const conn = MongoRunner.runMongod({setParameter: {enableComputeMode: true}});
const db = conn.getDB(jsTestName());

const kUrlProtocolFile = "file://";

// Create two random pipe names to avoid collisions with tests running concurrently on the same box.
const randomNum = Math.floor(1000 * 1000 * 1000 * Math.random());  // 0-999,999,999
const pipeName1 = "external_data_source_" + randomNum;
const pipeName2 = "external_data_source_" + (randomNum + 1);

// Empty option
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {$_externalDataSources: []});
}, 7039002);

// No external file metadata
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}],
                      {$_externalDataSources: [{collName: "coll", dataSources: []}]});
}, 7039001);

// No file type
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {
        $_externalDataSources: [{
            collName: "coll",
            dataSources: [{url: kUrlProtocolFile + pipeName1, storageType: "pipe"}]
        }]
    });
}, 40414);

// Unknown file type
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {
        $_externalDataSources: [{
            collName: "coll",
            dataSources:
                [{url: kUrlProtocolFile + pipeName1, storageType: "pipe", fileType: "unknown"}]
        }]
    });
}, ErrorCodes.BadValue);

// No storage type
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {
        $_externalDataSources: [
            {collName: "coll", dataSources: [{url: kUrlProtocolFile + pipeName1, fileType: "bson"}]}
        ]
    });
}, 40414);

// Unknown storage type
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {
        $_externalDataSources: [{
            collName: "coll",
            dataSources:
                [{url: kUrlProtocolFile + pipeName1, storageType: "unknown", fileType: "bson"}]
        }]
    });
}, ErrorCodes.BadValue);

// No url
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {
        $_externalDataSources:
            [{collName: "coll", dataSources: [{storageType: "pipe", fileType: "bson"}]}]
    });
}, 40414);

// Invalid url
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {
        $_externalDataSources: [{
            collName: "coll",
            dataSources: [{url: "http://abc.com/name1", storageType: "pipe", fileType: "bson"}]
        }]
    });
}, 6968500);

// The source namespace is not an external data source
assert.throwsWithCode(() => {
    db.unknown.aggregate([{$match: {a: 1}}], {
        $_externalDataSources: [{
            collName: "coll",
            dataSources:
                [{url: kUrlProtocolFile + pipeName1, storageType: "pipe", fileType: "bson"}]
        }]
    });
}, 7039003);

// $lookup's 'from' collection is not an external data source
assert.throwsWithCode(() => {
    db.coll.aggregate(
        [
            {$match: {a: 1}},
            {$lookup: {from: "unknown2", localField: "a", foreignField: "b", as: "out"}}
        ],
        {
            $_externalDataSources: [{
                collName: "coll",
                dataSources:
                    [{url: kUrlProtocolFile + pipeName1, storageType: "pipe", fileType: "bson"}]
            }]
        });
}, 7039004);

// Non-existing external data source
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {
        $_externalDataSources: [{
            collName: "coll",
            dataSources:
                [{url: kUrlProtocolFile + pipeName1, storageType: "pipe", fileType: "bson"}]
        }]
    });
}, ErrorCodes.FileNotOpen);

//
// Named Pipes success test cases follow.
//

////////////////////////////////////////////////////////////////////////////////////////////////////
// Test production code for MultiBsonStreamCursor and below, plus the shell pipe writer and reader
// functions, separately from $_externalDataSources to help narrow down the source of any failures.
////////////////////////////////////////////////////////////////////////////////////////////////////
let objsPerPipe = 25;
_writeTestPipe(pipeName1, objsPerPipe);
_writeTestPipe(pipeName2, objsPerPipe);
let result = _readTestPipes(pipeName1, pipeName2);
assert.eq((2 * objsPerPipe),
          bsonObjToArray(result)[0],  // "objects" first field contains the count of objects read
          "_readTestPipes read wrong number of objects: " + bsonObjToArray(result)[0]);

////////////////////////////////////////////////////////////////////////////////////////////////////
// Test that $_externalDataSource can read and aggregate multiple named pipes.
////////////////////////////////////////////////////////////////////////////////////////////////////
objsPerPipe = 100;
_writeTestPipe(pipeName1, objsPerPipe);
_writeTestPipe(pipeName2, objsPerPipe);
result = db.coll.aggregate([{$count: "objects"}], {
    $_externalDataSources: [{
        collName: "coll",
        dataSources: [
            {url: kUrlProtocolFile + pipeName1, storageType: "pipe", fileType: "bson"},
            {url: kUrlProtocolFile + pipeName2, storageType: "pipe", fileType: "bson"}
        ]
    }]
});
assert.eq((2 * objsPerPipe),
          result._batch[0].objects,  // shell puts agg result in "_batch"[0] field of a wrapper obj
          "$_externalDataSources read wrong number of objects: " + result._batch[0].objects);

////////////////////////////////////////////////////////////////////////////////////////////////////
// Test correctness by verifying reading from the pipes returns the same objects written to them.
////////////////////////////////////////////////////////////////////////////////////////////////////
const kObjsToWrite = [
    {"Zero": "zero zero zero zero zero zero zero zero zero zero zero zero zero zero zero zero"},
    {"One": "one one one one one one one one one one one one one one one one one one one one one"},
    {"Two": "two two two two two two two two two two two two two two two two two two two two two"},
    {"Three": "three three three three three three three three three three three three three"},
    {"Four": "four four four four four four four four four four four four four four four four"},
    {"Five": "five five five five five five five five five five five five five five five five"},
    {"Six": "six six six six six six six six six six six six six six six six six six six six six"}
];
const kNumObjs = kObjsToWrite.length;  // number of different objects for round-robin
const kPipes = 2;                      // number of pipes to write
_writeTestPipeObjects(pipeName1, objsPerPipe, kObjsToWrite);
_writeTestPipeObjects(pipeName2, objsPerPipe, kObjsToWrite);
let cursor = db.coll.aggregate([], {
    $_externalDataSources: [{
        collName: "coll",
        dataSources: [
            {url: kUrlProtocolFile + pipeName1, storageType: "pipe", fileType: "bson"},
            {url: kUrlProtocolFile + pipeName2, storageType: "pipe", fileType: "bson"}
        ]
    }]
});
// Verify the objects read from the pipes match what was written to them.
for (let pipe = 0; pipe < kPipes; ++pipe) {
    for (let obj = 0; obj < objsPerPipe; ++obj) {
        assert.eq(cursor.next(),
                  kObjsToWrite[obj % kNumObjs],
                  "Object read from pipe does not match expected.");
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Test successful lookup between two external data sources.
////////////////////////////////////////////////////////////////////////////////////////////////////
const kObjsToWrite1 = [{"localKey": 0}, {"localKey": 1}, {"localKey": 2}];
const kObjsToWrite2 =
    [{"foreignKey": 0, "foreignVal": "Zero"}, {"foreignKey": 2, "foreignVal": "Two"}];
const kExpectedLookup = [
    {"localKey": 0, "out": [{"foreignKey": 0, "foreignVal": "Zero"}]},
    {"localKey": 1, "out": []},
    {"localKey": 2, "out": [{"foreignKey": 2, "foreignVal": "Two"}]}
];

// $_externalDataSources specified in order (local, foreign).
_writeTestPipeObjects(pipeName1, kObjsToWrite1.length, kObjsToWrite1);  // local
_writeTestPipeObjects(pipeName2, kObjsToWrite2.length, kObjsToWrite2);  // foreign
let cursor1 = db.local.aggregate(
    [{$lookup: {from: "foreign", localField: "localKey", foreignField: "foreignKey", as: "out"}}], {
        $_externalDataSources: [
            {
                collName: "local",
                dataSources:
                    [{url: kUrlProtocolFile + pipeName1, storageType: "pipe", fileType: "bson"}]
            },
            {
                collName: "foreign",
                dataSources:
                    [{url: kUrlProtocolFile + pipeName2, storageType: "pipe", fileType: "bson"}]
            }
        ]
    });
// Verify the $lookup result.
for (let expected = 0; expected < kExpectedLookup.length; ++expected) {
    assert.eq(cursor1.next(),
              kExpectedLookup[expected],
              "Lookup result " + expected + " does not match expected.");
}

// $_externalDataSources specified in order (foreign, local).
_writeTestPipeObjects(pipeName1, kObjsToWrite1.length, kObjsToWrite1);  // local
_writeTestPipeObjects(pipeName2, kObjsToWrite2.length, kObjsToWrite2);  // foreign
let cursor2 = db.local.aggregate(
    [{$lookup: {from: "foreign", localField: "localKey", foreignField: "foreignKey", as: "out"}}], {
        $_externalDataSources: [
            {
                collName: "foreign",
                dataSources:
                    [{url: kUrlProtocolFile + pipeName2, storageType: "pipe", fileType: "bson"}]
            },
            {
                collName: "local",
                dataSources:
                    [{url: kUrlProtocolFile + pipeName1, storageType: "pipe", fileType: "bson"}]
            }
        ]
    });
// Verify the $lookup result.
for (let expected = 0; expected < kExpectedLookup.length; ++expected) {
    assert.eq(cursor2.next(),
              kExpectedLookup[expected],
              "Lookup result " + expected + " does not match expected.");
}

MongoRunner.stopMongod(conn);
})();
