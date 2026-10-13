/**
 * Reproducer for the FlatBSON lookup-map desync reachable through the validate command's
 * min/max replay on time-series buckets.
 *
 * FlatBSONStore keeps array element entries (which share one reserved field name) in the same
 * positional child list as named object fields. When an object field was merged onto an array
 * placeholder entry after the parent's fast lookup map had been built, the entry was renamed
 * without updating the map. The stale map missed the new name, a later insert created a
 * duplicate entry with that name, and the insert offset fixup counted the shared name twice, so
 * the map slot for that name drifted onto a different entry. Replayed by a MinMax recompute,
 * the update for one measurement field then landed on another field's entry. A heap-groomed
 * variant of the same sequence drifts the offset out of the child run entirely, which turned
 * the stale lookup into a use-after-free.
 *
 * On this branch the insert-time bucket validator does not recompute min/max, so the crafted
 * bucket below is stored. The validate command replays the data column through MinMax once per
 * measurement, and on an affected build it computes a wrong control.min (the minimum of 'z' is
 * clobbered by the later value of 'x', and 'x' appears twice in the computed min), so validate
 * reports the bucket non-compliant even though its control block holds the true element-wise
 * min and max.
 *
 * The bucket mixes object and array values at path 'm', which the current write path no longer
 * produces, so mixed-schema buckets are permitted here and the bucket is written directly to
 * the buckets namespace.
 *
 * @tags: [requires_fcv_70]
 */

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());
const collName = "ts";
const coll = db.getCollection(collName);
const bucketsColl = db.getCollection("system.buckets." + collName);

assert.commandWorked(db.createCollection(collName, {timeseries: {timeField: "t"}}));
// The bucket installed below mixes object and array values at one field path, so permit
// mixed-schema buckets. Only the validate command's min/max recompute is under test.
assert.commandWorked(
    db.runCommand({collMod: collName, timeseriesBucketsMayHaveMixedSchemaData: true}));

const t0 = ISODate("2026-01-01T00:00:00Z");
// The bucket _id must embed the same timestamp as control.min.t.
const bucketId =
    ObjectId(Math.floor(t0.getTime() / 1000).toString(16).padStart(8, "0") + "0000000000000000");

// Thirteen fields exceed the linear search limit, so the deep search miss below builds
// FlatBSON's field lookup map.
const wide = {};
for (let i = 0; i <= 12; i++) {
    wide["f" + i] = 0;
}

const measurements = [
    {...wide},
    // The array makes the computed max an array and appends an array placeholder entry that
    // shares the child run with the named fields.
    Array(14).fill(0),
    // The search for 'z' misses past the linear search limit and builds the lookup map.
    {z: 1},
    // The lock-step walk lands on the placeholder exactly when processing 'x' and claims it,
    // leaving the lookup map stale on affected builds.
    {z: 1, ...wide, x: 5},
    // The stale map has no key for 'x', so a duplicate 'x' entry is inserted and the insert
    // fixup counts the shared 'x' name twice, leaving the map slot for 'x' pointing at the 'z'
    // entry.
    {x: 7},
    // A consistent map resolves 'x' to its own entry and lowers its minimum to 0. The stale map
    // resolves 'x' to the 'z' entry and clobbers z's minimum instead.
    {z: 5, x: 0},
];

const dataT = {};
const dataM = {};
measurements.forEach((m, i) => {
    dataT[i] = t0;
    dataM[i] = m;
});

const bucket = {
    _id: bucketId,
    control: {
        version: NumberInt(1),
        // The true element-wise min and max over the measurements. Objects sort below arrays, so
        // only object values contribute to the min and only the array values to the max.
        min: {t: t0, m: {z: 1, ...wide, x: 0}},
        max: {t: t0, m: Array(14).fill(0)},
    },
    data: {t: dataT, m: dataM},
};

assert.commandWorked(bucketsColl.insertOne(bucket));

const res = coll.validate();
assert.commandWorked(res);
assert(res.valid, "validate reported a spurious min/max mismatch: " + tojson(res));
assert.eq(0, res.errors.length, tojson(res));

MongoRunner.stopMongod(conn);
