// Test that slaveOk is implicitly allowed for queries on a secondary with a read preference other
// than 'primary', and that queries which do have 'primary' read preference fail.
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
        for (let slaveOk of [true, false]) {
            const testType = {readMode: readMode, readPref: readPref, slaveOk: slaveOk};

            secDB.getMongo().forceReadMode(readMode);
            secDB.getMongo().setSlaveOk(slaveOk);

            const cursor = (readPref ? secDB.test.find().readPref(readPref) : secDB.test.find());

            if (readPref === "primary" || (!readPref && !slaveOk)) {
                // Attempting to run the query throws an error of type NotMasterNoSlaveOk.
                const slaveOkErr = assert.throws(() => cursor.itcount(), [], tojson(testType));
                assert.commandFailedWithCode(slaveOkErr, ErrorCodes.NotMasterNoSlaveOk);
            } else {
                // Succeeds for all non-primary readPrefs, and for no readPref iff slaveOk.
                const docCount = assert.doesNotThrow(() => cursor.itcount(), [], tojson(testType));
                assert.eq(docCount, 1);
            }
        }
    }
}

function assertNotMasterNoSlaveOk(func) {
    secDB.getMongo().forceReadMode("commands");
    secDB.getMongo().setSlaveOk(false);
    secDB.getMongo().setReadPref("primary");
    const res = assert.throws(func);
    assert.commandFailedWithCode(res, ErrorCodes.NotMasterNoSlaveOk);
}

// Test that agg with $out/$merge and non-inline mapReduce fail with 'NotMasterNoSlaveOk' when
// directed at a secondary with "primary" read preference.
const secondaryColl = secDB.slaveok_read_pref;
assertNotMasterNoSlaveOk(() => secondaryColl.aggregate([{$out: "target"}]).itcount());
assertNotMasterNoSlaveOk(
    () =>
        secondaryColl
            .aggregate([{$merge: {into: "target", whenMatched: "fail", whenNotMatched: "insert"}}])
            .itcount());
assertNotMasterNoSlaveOk(() => secondaryColl.mapReduce(() => emit(this.a),
                                                       (k, v) => Array.sum(b),
                                                       {out: {replace: "target"}}));

rst.stopSet();
})();
