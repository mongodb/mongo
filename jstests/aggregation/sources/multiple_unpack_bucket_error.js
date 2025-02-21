/**
 * Tests that an aggregation pipeline cannot have more than one $_internalUnpackBucket stage.
 */

const coll = db.multiple_unpack_bucket_error;
coll.drop();

assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {
            $_internalUnpackBucket:
                {exclude: [], timeField: 'time', bucketMaxSpanSeconds: NumberInt(3600)}
        },
        {
            $_internalUnpackBucket:
                {exclude: [], timeField: 'time', bucketMaxSpanSeconds: NumberInt(3600)}
        }
    ],
    cursor: {}
}),
                             7183900);

// $_unpackBucket is an alias of $_internalUnpackBucket, the same restriction should apply.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$_unpackBucket: {timeField: 'time'}},
        {
            $_internalUnpackBucket:
                {exclude: [], timeField: 'time', bucketMaxSpanSeconds: NumberInt(3600)}
        }
    ],
    cursor: {}
}),
                             7183900);
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {
            $_internalUnpackBucket:
                {exclude: [], timeField: 'time', bucketMaxSpanSeconds: NumberInt(3600)}
        },
        {$_unpackBucket: {timeField: 'time'}}
    ],
    cursor: {}
}),
                             7183900);
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$_unpackBucket: {timeField: 'time'}}, {$_unpackBucket: {timeField: 'time'}}],
    cursor: {}
}),
                             7183900);
