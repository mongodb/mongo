(function() {
"use strict";

const coll = db.cqf_find_limit_skip;
coll.drop();

const docCount = 50;
for (let i = 0; i < docCount; i++) {
    assert.commandWorked(coll.insert({a: 2}));
}

const invalidValueErrorCode = 51024;

// NEGATIVE LIMIT
// The mongo shell implements the logic specified in the documentation for negative limits, so the
// following command works as expected, i.e. the absolute value of the limit amount of documents is
// returned (since that is less than the default batch size).
let numReturned = coll.find().limit(-1).itcount();
assert.eq(numReturned, 1);

// This command will fail since a negative $limit field cannot be specified.
assert.commandFailedWithCode(coll.runCommand({find: coll.getName(), filter: {}, limit: -1}),
                             invalidValueErrorCode);

// Specify the batch size to ensure only one batch is returned with a negative limit. The default
// batch size is greater than the limit.
numReturned = coll.find().batchSize(3).limit(-10).itcount();
assert.eq(numReturned, 3);

// ZERO LIMIT
numReturned = coll.find().limit(0).itcount();
assert.eq(numReturned, docCount);

// Showing that the command that failed with a negative $limit passes when the limit is >= 0.
numReturned =
    coll.runCommand({find: coll.getName(), filter: {}, limit: 0})["cursor"]["firstBatch"].length;
assert.eq(numReturned, docCount);

// POSITIVE LIMIT
numReturned = coll.find().limit(10).itcount();
assert.eq(numReturned, 10);

// Specify the batch size to ensure that the amount of documents returned is the limit for a
// positive limit.
numReturned = coll.find().batchSize(3).limit(10).itcount();
assert.eq(numReturned, 10);

// NEGATIVE SKIP
assert.commandFailedWithCode(coll.runCommand({find: coll.getName(), filter: {}, skip: -1}),
                             invalidValueErrorCode);

// ZERO SKIP
numReturned = coll.find().skip(0).itcount();
assert.eq(numReturned, docCount);

// POSITIVE SKIP
numReturned = coll.find().skip(10).itcount();
assert.eq(numReturned, docCount - 10);

// Skip amount > docCount, should return 0 documents.
numReturned = coll.find().skip(60).itcount();
assert.eq(numReturned, 0);

// NEGATIVE LIMIT, ZERO SKIP
numReturned = coll.find().limit(-1).skip(0).itcount();
assert.eq(numReturned, 1);

// Specify the batch size to ensure only one batch is returned with a negative limit, zero skip.
numReturned = coll.find().batchSize(3).limit(-10).skip(0).itcount();
assert.eq(numReturned, 3);

// NEGATIVE LIMIT, POSITIVE SKIP
// Absolute value of limit amount > skip amount and docCount - skip amount > limit amount, should
// return the absolute value of the limit amount of documents. (Note: default batch size > absolute
// value of limit amount and default batch size > skip amount)
let limitAmount = -10;
let skipAmount = 1;
numReturned = coll.find().limit(limitAmount).skip(skipAmount).itcount();
assert.eq(numReturned, Math.abs(limitAmount));

// Absolute value of limit amount > skip amount and docCount - skip amount < limit amount, should
// return (docCount - skip amount) documents.
limitAmount = -45;
skipAmount = 40;
numReturned = coll.find().limit(limitAmount).skip(skipAmount).itcount();
assert.eq(numReturned, docCount - skipAmount);

// Absolute value of limit amount > skip amount > batch size, should return one batch of documents.
numReturned = coll.find().batchSize(3).limit(-10).skip(5).itcount();
assert.eq(numReturned, 3);

// Absolute value of limit amount > batch size > skip amount and batch size < docCount - skip
// amount, should return one batch of documents.
numReturned = coll.find().batchSize(5).limit(-10).skip(3).itcount();
assert.eq(numReturned, 5);

// Batch size > docCount - skip amount, should return the absolute value of the limit number of
// documents.
limitAmount = -10;
skipAmount = 3;
let batchSize = 45;
numReturned = coll.find().batchSize(batchSize).limit(limitAmount).skip(skipAmount).itcount();
assert.eq(numReturned, Math.abs(limitAmount));

// Skip amount > batch size and docCount - limit amount > docCount - skip amount, should return
// (docCount - skip amount) documents.
limitAmount = -10;
skipAmount = 47;
batchSize = 45;
numReturned = coll.find().batchSize(batchSize).limit(limitAmount).skip(skipAmount).itcount();
assert.eq(numReturned, docCount - skipAmount);

// Absolute value of limit amount < skip amount, should return the absolute value of the limit
// amount of documents.
numReturned = coll.find().limit(-1).skip(10).itcount();
assert.eq(numReturned, 1);

// Batch size < absolute value of limit amount < skip amount < docCount, should return one batch of
// documents.
numReturned = coll.find().batchSize(3).limit(-5).skip(10).itcount();
assert.eq(numReturned, 3);

// Absolute value of limit amount = skip amount < docCount, should return the absolute value of the
// limit amount of documents.
numReturned = coll.find().limit(-10).skip(10).itcount();
assert.eq(numReturned, 10);

// Absolute value of limit amount = skip amount = batchsize < docCount, should return one batch of
// documents.
numReturned = coll.find().batchSize(10).limit(-10).skip(10).itcount();
assert.eq(numReturned, 10);

// Absolute value of limit amount = skip amount = batchsize > docCount, should return 0 documents.
numReturned = coll.find().batchSize(60).limit(-60).skip(60).itcount();
assert.eq(numReturned, 0);

// ZERO LIMIT, ZERO SKIP
numReturned = coll.find().limit(0).skip(0).itcount();
assert.eq(numReturned, docCount);

// POSITIVE LIMIT, ZERO SKIP
numReturned = coll.find().limit(10).skip(0).itcount();
assert.eq(numReturned, 10);

// ZERO LIMIT, POSITIVE SKIP
numReturned = coll.find().limit(0).skip(10).itcount();
assert.eq(numReturned, docCount - 10);

// POSITIVE LIMIT, POSITIVE SKIP
// Limit amount > skip amount, should return (limit) documents.
numReturned = coll.find().limit(10).skip(1).itcount();
assert.eq(numReturned, 10);

// Limit amount < skip amount, should return (limit) documents.
numReturned = coll.find().limit(1).skip(10).itcount();
assert.eq(numReturned, 1);

// Limit amount = skip amount, should return (limit) documents.
numReturned = coll.find().limit(10).skip(10).itcount();
assert.eq(numReturned, 10);

// (docCount - skip amount) < limit, should return (docCount - skip amount) documents.
limitAmount = 40;
skipAmount = 15;
numReturned = coll.find().limit(limitAmount).skip(skipAmount).itcount();
assert.eq(numReturned, docCount - skipAmount);
}());
