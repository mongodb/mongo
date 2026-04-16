/**
 * Tests that an $elemMatch on an empty path is handled correctly.
 * Reproduces SERVER-121207.
 * @tags: [requires_fcv_90]
 */
const coll = db[jsTestName()];
assert(coll.drop());

assert.commandWorked(coll.createIndex({"t": 1}));
assert.commandWorked(coll.insertMany([{}, {t: 1, "": [{a: 1}]}]));

const matchPred = {
    t: 1,
    "": {
        $elemMatch: {
            $or: [{a: 1}, {b: 1}],
        },
    },
};

const res = coll.find(matchPred).toArray();
assert.eq(res.length, 1);
assert.eq(res[0].t, 1);
