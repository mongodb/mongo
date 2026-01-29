// @tags: [requires_fcv_83, requires_sbe]

const coll = db.bucket_auto_pow2_inf;
coll.drop();

assert.commandWorked(coll.insert({_id: 0}));

// Construct a groupBy expression that overflows to +Infinity on the server.
//
// 1e308 is a large but finite double. The $add is evaluated by the server, not
// by the shell, so the server will see two finite doubles and add them in
// C++ as `double`, overflowing to +Infinity (IEEE-754).
const pipeline = [
    {
        $bucketAuto: {
            groupBy: {$add: [1e308, 1e308]},
            buckets: 2,
            granularity: "POWERSOF2",
            output: {count: {$sum: 1}},
        },
    },
];

assert.commandFailedWithCode(coll.runCommand("aggregate", {pipeline: pipeline, cursor: {}}), 11785400);
