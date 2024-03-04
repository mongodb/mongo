/**
 * Test consistency of query solution hashes by extracting from the plan cache.
 *
 * @tags: [
 *   # TODO SERVER-85728: Enable Bonsai plan cache tests involving indices.
 *   cqf_incompatible,
 *   # Plan cache state is node-local and will not get migrated alongside user data.
 *   tenant_migration_incompatible,
 *   assumes_balancer_off,
 *   # $planCacheStats requires readConcern local and reading from the same node.
 *   assumes_read_concern_local,
 *   assumes_read_concern_unchanged,
 *   assumes_read_preference_unchanged,
 *   # $planCacheStats cannot run within a multi-document transaction.
 *   does_not_support_transactions,
 *   # planCacheClear command may return different values after a failover.
 *   does_not_support_stepdowns,
 *   # planCacheClear is not allowed with a security token.
 *   not_allowed_with_signed_security_token,
 *   # Implicit index creation may change the plan used.
 *   assumes_no_implicit_index_creation,
 *   # This test checks a new field "solutionHash" in $planCacheStats, not available in previous
 *   # versions.
 *   requires_fcv_72,
 *   multiversion_incompatible,
 * ]
 */

import {checkCascadesOptimizerEnabled} from "jstests/libs/optimizer_utils.js";

(function() {
const coll = db.query_solution_hash;
coll.drop();

assert.commandWorked(coll.insert({a: 1, b: 1}));

function getCachedSolutionHash() {
    const cache = coll.getPlanCache().list();
    return cache[0].solutionHash;
}

/*
 * Run a query until it's cached. Remember the solution hash, then clear the cache. Then check that
 * the second time the query is cached, it has the same hash.
 */
function sameHashAfterCacheDrop(queryFunc) {
    coll.getPlanCache().clear();
    for (let i = 0; i < 5; i++) {
        queryFunc(1).toArray();
    }
    // If there's no cache entry, or multiple, we move on.
    if (coll.getPlanCache().list().length !== 1) {
        return;
    }
    const hash = getCachedSolutionHash();

    coll.getPlanCache().clear();
    for (let i = 0; i < 5; i++) {
        queryFunc(2).toArray();
    }
    if (coll.getPlanCache().list().length !== 1) {
        return;
    }
    assert.eq(hash, getCachedSolutionHash(), () => tojson(coll.getPlanCache().list()));
}

function testSameSolutionHash() {
    const queries = [
        (param) => coll.find({a: param, b: 1}),
        (param) => coll.find({a: param}, {b: 1}),
        () => coll.find({}, {a: 1, b: 1}),
        (param) => coll.aggregate([{$match: {a: param, b: 1}}]),
        () => coll.aggregate([{$project: {_id: 0, a: 1, b: 1}}]),
        (param) => coll.aggregate([{$match: {a: param, b: 1}}, {$project: {_id: 0, a: 1, b: 1}}]),
        (param) => coll.aggregate([{$match: {a: param, b: 1}}, {$sort: {a: 1, b: 1}}]),
        () => coll.aggregate([{$project: {a: 1, b: 1}}, {$sort: {a: 1, b: 1}}]),
    ];
    queries.forEach(sameHashAfterCacheDrop);
}

// TODO SERVER-85728: Enable Bonsai plan cache tests involving indices.
if (checkCascadesOptimizerEnabled(db)) {
    // We don't support the other cases when indexes are present.

    // Collscan case
    testSameSolutionHash();
} else {
    // Collscan case
    testSameSolutionHash();

    // Irrelevant index case
    assert.commandWorked(coll.createIndex({c: 1}));
    testSameSolutionHash();

    // Useful index case
    assert.commandWorked(coll.createIndex({a: 1}));
    testSameSolutionHash();

    // Covering index case
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    testSameSolutionHash();

    // Test that same queries with different collation have different query plan hashes.
    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}, {name: "b1", collation: {locale: 'fr_CA'}}));
    assert.commandWorked(coll.createIndex({b: 1}, {name: "b2", collation: {locale: 'en_US'}}));

    // It should choose the "a" index.
    for (let i = 0; i < 100; i++) {
        assert.commandWorked(coll.insert({a: i, b: 'foo'}));
    }

    for (let i = 0; i < 2; i++) {
        coll.find({a: 5, b: 'foo'}).collation({locale: 'fr_CA'}).toArray();
        coll.find({a: 5, b: 'foo'}).collation({locale: 'en_US'}).toArray();
    }

    const cache = coll.getPlanCache().list();
    assert.eq(cache.length, 2);
    assert.neq(cache[0].solutionHash, cache[1].solutionHash);
}
})();
