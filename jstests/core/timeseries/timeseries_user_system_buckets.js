/**
 * Technically this is not time series colleciton test; however, due to legacy behavior, a user
 * inserts into a collection in time series bucket namespace is required not to fail.  Please note
 * this behavior is likely going to be corrected in 6.3 or after. The presence of this test does
 * not imply such behavior is supported.
 *
 * As this tests code path relevant to time series, the requires_tiemseries flag is set to avoid
 * incompatible behavior related to multi statement transactions.
 *
 *  @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_fcv_63
 * ]
 */
userCollSystemBuckets = db.system.buckets.coll;
userColl = db.coll;

userCollSystemBuckets.drop();
userColl.drop();

// inserting into a user defined system buckets collection is possible
assert.commandWorked(userCollSystemBuckets.insert({a: 1}));

// A user collection with the same postfix should not be considered time series collection
assert.commandWorked(userColl.insert({a: 2}));

docs = userColl.find().toArray();
assert.eq(1, docs.length);

docsSystemBuckets = userCollSystemBuckets.find().toArray();
assert.eq(1, docsSystemBuckets.length);

userCollSystemBuckets.drop();
userColl.drop();

// the sequence in different order should also work
assert.commandWorked(userColl.insert({a: 2}));
assert.commandWorked(userCollSystemBuckets.insert({a: 1}));

docs = userColl.find().toArray();
assert.eq(1, docs.length);

docsSystemBuckets = userCollSystemBuckets.find().toArray();
assert.eq(1, docsSystemBuckets.length);

userCollSystemBuckets.drop();
userColl.drop();
