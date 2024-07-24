/**
 * Test a rooted $or query with duplicate predicates which reproduces SERVER-84338, a bug in the
 * MatchExpression $or->$in rewrite which produced incorrect plans in the SBE plan cache.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: planCacheClear.
 *   not_allowed_with_signed_security_token,
 *   # This test attempts to perform queries and introspect/manipulate the server's plan cache
 *   # entries. The former operation may be routed to a secondary in the replica set, whereas the
 *   # latter must be routed to the primary.
 *   assumes_read_concern_unchanged,
 *   assumes_read_preference_unchanged,
 *   does_not_support_stepdowns,
 *   # If all chunks are moved off of a shard, it can cause the plan cache to miss commands.
 *   assumes_balancer_off,
 *   assumes_unsharded_collection,
 *   # Plan cache state is node-local and will not get migrated alongside tenant data.
 *   tenant_migration_incompatible,
 * ]
 */

const coll = db.server84338;
coll.drop();

const docs = [
    {_id: 1, foo: 1},
    {_id: 2, foo: 2},
];
assert.commandWorked(coll.insert(docs));

const predicate = {
    $or: [{_id: 1, foo: 1}, {_id: 1, foo: 999}]
};
const predicate2 = {
    $or: [{_id: 2, foo: 2}, {_id: 2, foo: 999}]
};

// This query places an entry in the SBE plan cache.
assert.eq([{_id: 1, foo: 1}], coll.find(predicate).toArray());
// This query reuses this entry from the cache.
assert.eq([{_id: 2, foo: 2}], coll.find(predicate2).toArray());

coll.getPlanCache().clear();

assert.eq(
    [{_id: null, s: 1}],
    coll.aggregate([{$match: predicate}, {$group: {_id: null, s: {$sum: "$foo"}}}]).toArray());
assert.eq(
    [{_id: null, s: 2}],
    coll.aggregate([{$match: predicate2}, {$group: {_id: null, s: {$sum: "$foo"}}}]).toArray());

// Tests that $or queries eligible for the $or-to-$in rewrite where the constants contain some
// non-parameterizable constants do not fail. This is a regression test for SERVER-92603.

// No predicates are parameterizable.
assert.eq([], coll.find({$or: [{_id: 1, foo: true}, {_id: 1, foo: false}]}).toArray());
// Some predicates are parameterizable.
assert.eq(
    [{_id: 1, foo: 1}],
    coll.find({$or: [{_id: 1, foo: 1}, {_id: 1, foo: true}, {_id: 1, foo: false}]}).toArray());
