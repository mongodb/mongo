// Test that secondaryOk is implicitly allowed for queries on a secondary with a read preference
// other than 'primary', and that queries which do have 'primary' read preference fail.
(function() {
"use strict";

const readPrefs =
    [undefined, "primary", "secondary", "primaryPreferred", "secondaryPreferred", "nearest"];

const rst = new ReplSetTest({nodes: 3});
rst.startSet();

const nodes = rst.nodeList();
rst.initiate({
    _id: jsTestName(),
    members: [
        {_id: 0, host: nodes[0]},
        {_id: 1, host: nodes[1], priority: 0},
        {_id: 2, host: nodes[2], arbiterOnly: true}
    ]
});

const priDB = rst.getPrimary().getDB(jsTestName());
assert(priDB.dropDatabase());

assert.commandWorked(priDB.test.insert({a: 1}, {writeConcern: {w: "majority"}}));

const secDB = rst.getSecondary().getDB(jsTestName());

for (let readMode of ["commands", "legacy"]) {
    for (let readPref of readPrefs) {
        for (let secondaryOk of [true, false]) {
            const testType = {readMode: readMode, readPref: readPref, secondaryOk: secondaryOk};

            secDB.getMongo().forceReadMode(readMode);
            secDB.getMongo().setSecondaryOk(secondaryOk);

            const cursor = (readPref ? secDB.test.find().readPref(readPref) : secDB.test.find());

            if (readPref === "primary" || (!readPref && !secondaryOk)) {
                // Attempting to run the query throws an error of type NotPrimaryNoSecondaryOk.
                const secondaryOkErr = assert.throws(() => cursor.itcount(), [], tojson(testType));
                assert.commandFailedWithCode(secondaryOkErr, ErrorCodes.NotPrimaryNoSecondaryOk);
            } else {
                // Succeeds for all non-primary readPrefs, and for no readPref iff secondaryOk.
                const docCount = assert.doesNotThrow(() => cursor.itcount(), [], tojson(testType));
                assert.eq(docCount, 1);
            }
        }
    }
}

function assertNotPrimaryNoSecondaryOk(func) {
    secDB.getMongo().forceReadMode("commands");
    secDB.getMongo().setSecondaryOk(false);
    secDB.getMongo().setReadPref("primary");
    const res = assert.throws(func);
    assert.commandFailedWithCode(res, ErrorCodes.NotPrimaryNoSecondaryOk);
}

// Test that agg with $out/$merge and non-inline mapReduce fail with 'NotPrimaryNoSecondaryOk' when
// directed at a secondary with "primary" read preference.
const secondaryColl = secDB.secondaryok_read_pref;
assertNotPrimaryNoSecondaryOk(() => secondaryColl.aggregate([{$out: "target"}]).itcount());
assertNotPrimaryNoSecondaryOk(
    () =>
        secondaryColl
            .aggregate([{$merge: {into: "target", whenMatched: "fail", whenNotMatched: "insert"}}])
            .itcount());
assertNotPrimaryNoSecondaryOk(() => secondaryColl.mapReduce(() => emit(this.a),
                                                            (k, v) => Array.sum(b),
                                                            {out: {replace: "target"}}));

rst.stopSet();
})();
