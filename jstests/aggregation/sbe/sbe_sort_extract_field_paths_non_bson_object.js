/**
 * When run in SBE, extract_field_paths iterates over the Object
 * typeTag for `m` to calculate `$m.m1`. Tests we don't tassert.
 */
assert.commandWorked(db.c.createIndex({"m.m1": 1}));
assert.commandWorked(
    db.c.insertOne({
        "m": {"m1": NumberInt(0), "m2": NumberInt(0)},
    }),
);
assert.eq(db.c.aggregate([{"$sort": {"m.m1": 1}}, {"$group": {"_id": null, "a": {"$min": "$m.m1"}}}]).toArray(), [
    {"_id": null, "a": NumberInt(0)},
]);
