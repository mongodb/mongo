/**
 * Repros a tassert where $sort stage builders would produce a dotted path in a
 * kField slot and then the extract_field_paths stage builder would be
 * surprised that there is a dotted path in a kField slot.
 */
const query = [
    {
        "$project": {
            "_id": 0,
            "m.m2": 1,
        },
    },
    {
        "$addFields": {
            "a": "$m.m1",
        },
    },
    {
        "$sort": {
            "m.m2": 1,
        },
    },
    {
        "$project": {
            "_id": 0,
            "a": 1,
        },
    },
];
const coll = db.coll;
coll.drop();
coll.insert({});
coll.createIndex({"m.m2": 1, a: 1});
coll.aggregate(query, {hint: "m.m2_1_a_1"}).toArray();
