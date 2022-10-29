/**
 * Tests basic syntax check of $_externalDataSources aggregate command option.
 */
(function() {
"use strict";

// Runs tests on a standalone mongod.
const conn = MongoRunner.runMongod({setParameter: {enableComputeMode: true}});
const db = conn.getDB(jsTestName());

const kUrlProtocolFile = "file://";

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
            dataSources: [{url: kUrlProtocolFile + "name1", storageType: "pipe"}]
        }]
    });
}, 40414);

// Unknown file type
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {
        $_externalDataSources: [{
            collName: "coll",
            dataSources:
                [{url: kUrlProtocolFile + "name1", storageType: "pipe", fileType: "unknown"}]
        }]
    });
}, ErrorCodes.BadValue);

// No storage type
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {
        $_externalDataSources:
            [{collName: "coll", dataSources: [{url: kUrlProtocolFile + "name1", fileType: "bson"}]}]
    });
}, 40414);

// Unknown storage type
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {
        $_externalDataSources: [{
            collName: "coll",
            dataSources:
                [{url: kUrlProtocolFile + "name1", storageType: "unknown", fileType: "bson"}]
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
            dataSources: [{url: kUrlProtocolFile + "name1", storageType: "pipe", fileType: "bson"}]
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
                    [{url: kUrlProtocolFile + "name1", storageType: "pipe", fileType: "bson"}]
            }]
        });
}, 7039004);

// Non-existing external data source
assert.throwsWithCode(() => {
    db.coll.aggregate([{$match: {a: 1}}], {
        $_externalDataSources: [{
            collName: "coll",
            dataSources: [{url: kUrlProtocolFile + "name1", storageType: "pipe", fileType: "bson"}]
        }]
    });
}, ErrorCodes.FileNotOpen);

MongoRunner.stopMongod(conn);
})();
