/**
 * This test tests concurrent read and write behavior for tailable cursors on unreplicated capped
 * collections. These collections accept concurrent writes and thus must ensure no documents are
 * skipped for forward cursors.
 *
 * This test sets up a single capped collection with many concurrent writers. Concurrent readers
 * open tailable cursors and clone the contents into their own collection copies. The readers then
 * assert that the contents match the source.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   does_not_support_retryable_writes,
 *   requires_capped,
 *   requires_non_retryable_writes,
 *   # Tailable cursors do not work correctly on previous versions.
 *   requires_fcv_63,
 * ]
 */

load("jstests/libs/parallelTester.js");  // For Thread

(function() {
'use strict';

function insertWorker(host, collName, tid, nInserts) {
    const conn = new Mongo(host);
    const db = conn.getDB('local');

    for (let i = 0; i < nInserts;) {
        const bulk = db[collName].initializeUnorderedBulkOp();
        for (let j = 0; j < 10; j++) {
            bulk.insert({t: tid, i: i++});
        }
        assert.commandWorked(bulk.execute());
    }
    print(tid + ": done");
}

function tailWorker(host, collName, tid, expectedDocs) {
    // Rewrite the connection string as a mongo URI so that we can add an 'appName' to make
    // debugging easier. When run against a standalone, 'host' is in the form '<host>:<port>'. When
    // run against a replica set, 'host' is in the form '<rs name>/<host1>:<port1>,...'
    const iSlash = host.indexOf('/');
    let connString = 'mongodb://';
    if (iSlash > 0) {
        connString += host.substr(iSlash + 1) + '/?appName=tid' + tid +
            '&replicaSet=' + host.substr(0, iSlash);
    } else {
        connString += host + '/?appName=tid' + tid;
    }
    const conn = new Mongo(connString);
    const db = conn.getDB('local');
    const cloneColl = db[collName + "_clone_" + tid];
    cloneColl.drop();

    let res = db.runCommand({find: collName, batchSize: 0, awaitData: true, tailable: true});
    assert.commandWorked(res);
    assert.gt(res.cursor.id, NumberLong(0));
    assert.eq(res.cursor.firstBatch.length, 0);

    const curId = res.cursor.id;

    let myCount = 0;
    let emptyGetMores = 0;
    let nonEmptyGetMores = 0;
    assert.soon(() => {
        res = db.runCommand({getMore: curId, collection: collName, maxTimeMS: 1000});
        assert.commandWorked(res);

        const batchLen = res.cursor.nextBatch.length;
        if (batchLen > 0) {
            nonEmptyGetMores++;
        } else {
            emptyGetMores++;
        }

        print(tid + ': got batch of size ' + batchLen +
              '. first doc: ' + tojson(res.cursor.nextBatch[0]) +
              '. last doc: ' + tojson(res.cursor.nextBatch[batchLen - 1]) +
              '. empty getMores so far: ' + emptyGetMores +
              '. non-empty getMores so far: ' + nonEmptyGetMores);
        myCount += batchLen;

        const bulk = cloneColl.initializeUnorderedBulkOp();
        for (let i = 0; i < batchLen; i++) {
            bulk.insert(res.cursor.nextBatch[i]);
        }
        assert.commandWorked(bulk.execute());

        // The writers are done, so we are draining until we see as many docs as we
        // expect.
        if (myCount == expectedDocs) {
            return true;
        } else {
            print(tid + ": waiting. my count: " + myCount + " expected: " + expectedDocs);
        }
        return false;
    }, "failed to return all documents within timeout");

    print(tid + ": validating");
    const expected = db[collName].find().sort({_id: 1}).toArray();
    const actual = cloneColl.find().sort({_id: 1}).toArray();
    assert.eq(expected.length, actual.length, function() {
        return "number of documents do not match. expected: " + tojson(expected) +
            " actual: " + tojson(actual);
    });
    for (let i = 0; i < actual.length; i++) {
        assert.docEq(actual[i], expected[i], function() {
            return "mismatched documents. expected: " + tojson(expected) +
                " actual: " + tojson(actual);
        });
    }
    print(tid + ": done");
}

const collName = 'capped';
const localDb = db.getSiblingDB('local');
localDb[collName].drop();

assert.commandWorked(localDb.runCommand({create: collName, capped: true, size: 10 * 1024 * 1024}));
assert.commandWorked(localDb[collName].insert({firstDoc: 1, i: -1}));

const nWriters = 5;
const nReaders = 5;

const insertsPerThread = 1000;
const expectedDocs = nWriters * insertsPerThread + 1;

let threads = [];

for (let i = 0; i < nReaders; i++) {
    const thread =
        new Thread(tailWorker, db.getMongo().host, collName, threads.length, expectedDocs);
    thread.start();
    threads.push(thread);
}

for (let i = 0; i < nWriters; i++) {
    const thread =
        new Thread(insertWorker, db.getMongo().host, collName, threads.length, insertsPerThread);
    thread.start();
    threads.push(thread);
}

for (let i = 0; i < threads.length; i++) {
    threads[i].join();
}
})();
