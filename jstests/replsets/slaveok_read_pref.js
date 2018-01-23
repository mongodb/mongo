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

    assert.writeOK(priDB.test.insert({a: 1}, {writeConcern: {w: "majority"}}));

    const secDB = rst.getSecondary().getDB(jsTestName());

    for (let readMode of["commands", "legacy"]) {
        for (let readPref of readPrefs) {
            for (let slaveOk of[true, false]) {
                const testType = {readMode: readMode, readPref: readPref, slaveOk: slaveOk};

                secDB.getMongo().forceReadMode(readMode);
                secDB.getMongo().setSlaveOk(slaveOk);

                const cursor =
                    (readPref ? secDB.test.find().readPref(readPref) : secDB.test.find());

                if (readPref === "primary" || (!readPref && !slaveOk)) {
                    // Attempting to run the query throws an error of type NotMasterNoSlaveOk.
                    const slaveOkErr = assert.throws(() => cursor.itcount(), [], testType);
                    assert.commandFailedWithCode(slaveOkErr, ErrorCodes.NotMasterNoSlaveOk);
                } else {
                    // Succeeds for all non-primary readPrefs, and for no readPref iff slaveOk.
                    const docCount = assert.doesNotThrow(() => cursor.itcount(), [], testType);
                    assert.eq(docCount, 1);
                }
            }
        }
    }

    rst.stopSet();
})();