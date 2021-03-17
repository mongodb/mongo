/**
 * Tests that an aggregation pipeline cannot have more than one $_internalUnpackBucket stage.
 *
 * @tags: [ do_not_wrap_aggregations_in_facets ]
 */

(function() {
"use strict";

const coll = db.multiple_unpack_bucket_error;
coll.drop();

assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$_internalUnpackBucket: {exclude: [], timeField: 'time', bucketMaxSpanSeconds: 3600}},
        {$_internalUnpackBucket: {exclude: [], timeField: 'time', bucketMaxSpanSeconds: 3600}}
    ],
    cursor: {}
}),
                             5348302);
})();
