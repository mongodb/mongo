/**
 *
 * Tests TTL behavior when there are collections with multiple TTL indexes and multiple collections
 * with TTL indexes. It's important that all TTL indexes are given a 'fair' amount of time to delete
 * expired documents, and that one TTL index or collection with TTL indexes does not starve others
 * by deleting a large amount of documents uninterruptedly.
 *
 * Tests both the 'batched' and 'legacy' TTL deletes behavior. When batching is enabled, fairness
 * constraints are maintained to limit the amount of time and resources dedicated to a single TTL
 * index. When batching is disabled, TTL deletes use legacy behavior where a single pass iterates
 * over each TTL index and deletes as much as possible for each index, regardless of the time it
 * takes.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   requires_ttl_index,
 *   # Expects TTL parameters 'ttlIndexDeleteTargetDocs' and 'ttlMonitorSubPassTargetSecs'.
 *   requires_fcv_61,
 * ]
 */
(function() {
"use strict";

Random.setRandomSeed();

// Significantly reduce the number of ttlIndexDeleteTargetDocs and ttlMonitorSubPassTargetSecs to
// increase the likelihood that a pass consists of multiple sub-passes.
const ttlMonitorSleepSecs = 5;
const ttlMonitorSubPassTargetSecs = 1;
const ttlIndexDeleteTargetDocs = 50;

const params = {
    ttlMonitorSleepSecs,
    ttlMonitorSubPassTargetSecs,
    ttlIndexDeleteTargetDocs,
};

const conn = MongoRunner.runMongod({setParameter: params});

const expireAfterSeconds = 1;
const db = conn.getDB(jsTestName());
const collNameA = "collA";
const collNameB = "collB";
const indexKeyX = "x";
const indexKeyY = "y";
const xExpiredInfo = "xExpired";
const yExpiredInfo = "yExpired";

function populateExpiredDocs(now, coll, indexKey, info, numExpiredDocs) {
    const docs = [];
    for (let i = 1; i <= numExpiredDocs; i++) {
        const tenTimesExpiredMs = 10 * expireAfterSeconds * 1000;
        const pastDate = new Date(now - tenTimesExpiredMs - i);
        docs.push({[indexKey]: pastDate, info});
    }
    assert.commandWorked(coll.insertMany(docs, {ordered: false}));
}
function runTests(batchingEnabled) {
    // If batching is disabled, the TTL Monitor runs with legacy behavior, meaning the concept of a
    // sub-pass, 'ttlMonitorSubPassTargetSecs', and 'ttlIndexDeleteTargetDocs' hold no meaning.
    jsTest.log(`Running TTL tests with batching enabled? ${batchingEnabled}`);
    assert.commandWorked(
        conn.getDB('admin').runCommand({setParameter: 1, ttlMonitorBatchDeletes: batchingEnabled}));

    // Test two TTL indexes on the same collection.
    {
        jsTest.log("Testing TTL indexes on the same collection");
        const collA = db[collNameA];
        collA.drop();

        // All documents will be expired the same time between 'now' and some interval in the past.
        const now = new Date();

        // Start out with significantly more expired documents on one index than
        // the other.
        populateExpiredDocs(now, collA, indexKeyX, xExpiredInfo, ttlIndexDeleteTargetDocs * 500);
        populateExpiredDocs(now, collA, indexKeyY, yExpiredInfo, 1);

        assert.commandWorked(collA.createIndex({[indexKeyX]: 1}, {expireAfterSeconds}));
        assert.commandWorked(collA.createIndex({[indexKeyY]: 1}, {expireAfterSeconds}));

        assert.soon(() => {
            // Only wait for the TTL index with the least amount of expired documents to be emptied
            // before adding more expired documents to the index. This makes it possible, in some
            // test runs, to add more expired documents before the current pass is complete -
            // demonstrating that eventually everything will be expired whether it is the current
            // pass or the next.
            return collA.find({"info": yExpiredInfo}).itcount() == 0;
        });

        // The server status logged may be stale by the next operation.
        jsTest.log(`TTL indexes on same collection -- TTL server status 0: ${
            tojson(db.serverStatus().metrics.ttl)}`);

        // At a minimum, a sub-pass has removed expired documents on indexKeyY. Insert more expired
        // documents to show the TTL Monitor eventually deletes more on indexKeyY. Note - it is
        // possible the initial TTL pass has yet to complete and there are still expired documents
        // on indexKeyX at this time (provided batching is enabled).
        populateExpiredDocs(now, collA, indexKeyY, yExpiredInfo, ttlIndexDeleteTargetDocs * 50);

        assert.soon(() => {
            return collA.find({"info": xExpiredInfo}).itcount() == 0 &&
                collA.find({"info": yExpiredInfo}).itcount() == 0;
        });

        jsTest.log(`TTL indexes on same collection -- TTL server status 1: ${
            tojson(db.serverStatus().metrics.ttl)}`);
    }

    // Test TTL indexes on different collections.
    {
        jsTest.log("Testing TTL indexes on different collections");
        const collA = db[collNameA];
        const collB = db[collNameB];

        collA.drop();
        collB.drop();

        // All documents will be expired the same time between 'now' and some interval in the past.
        const now = new Date();

        // Start out with significantly more expired documents on one collection than the other.
        populateExpiredDocs(now, collA, indexKeyX, xExpiredInfo, ttlIndexDeleteTargetDocs * 500);
        populateExpiredDocs(now, collB, indexKeyX, xExpiredInfo, 1);

        assert.commandWorked(collA.createIndex({[indexKeyX]: 1}, {expireAfterSeconds}));
        assert.commandWorked(collB.createIndex({[indexKeyX]: 1}, {expireAfterSeconds}));

        assert.soon(() => {
            // Only wait for the TTL index with the least amount of expired documents to be emptied
            // before adding a new index with expired documents. This makes it possible, in some
            // test runs, to add more expired documents before the current pass is complete -
            // demonstrating that eventually everything will be expired whether it is the current
            // pass or the next.
            return collB.find({"info": xExpiredInfo}).itcount() == 0;
        });

        // The server status logged may be stale by the next operation.
        jsTest.log(`TTL indexes on different collections -- TTL server status 0: ${
            tojson(db.serverStatus().metrics.ttl)}`);

        // Ensure a new index isn't starved.
        populateExpiredDocs(now, collB, indexKeyY, yExpiredInfo, ttlIndexDeleteTargetDocs * 50);
        assert.commandWorked(collB.createIndex({[indexKeyY]: 1}, {expireAfterSeconds}));

        assert.soon(() => {
            return collA.find({"info": xExpiredInfo}).itcount() == 0 &&
                collB.find({"info": xExpiredInfo}).itcount() == 0 &&
                collB.find({"info": yExpiredInfo}).itcount() == 0;
        });

        jsTest.log(`TTL indexes on different collections -- TTL server status 1: ${
            tojson(db.serverStatus().metrics.ttl)}`);
    }
}

runTests(true /** batchingEnabled **/);
runTests(false /** batchingEnabled **/);
MongoRunner.stopMongod(conn);
})();
