/**
 * Tests that CWWC cannot be unset once it is set.
 * @tags: [
 * ]
 */
(function() {
"use strict";

function runTest(conn) {
    let expectedDefaultWC = {w: "majority", wtimeout: 0};
    let res = conn.adminCommand({getDefaultRWConcern: 1});
    assert(res.hasOwnProperty("defaultWriteConcern"));
    assert.eq(expectedDefaultWC, res.defaultWriteConcern, tojson(res));

    jsTestLog("Setting the default write concern to empty initially works.");
    assert.commandWorked(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {},
    }));

    res = conn.adminCommand({getDefaultRWConcern: 1});
    assert(res.hasOwnProperty("defaultWriteConcern"));
    assert.eq(expectedDefaultWC, res.defaultWriteConcern, tojson(res));

    jsTestLog("Setting the default write concern.");
    const newDefaultWriteConcern = {w: 2, wtimeout: 60};
    assert.commandWorked(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: newDefaultWriteConcern,
    }));
    res = conn.adminCommand({getDefaultRWConcern: 1});
    assert.eq(res.defaultWriteConcern, newDefaultWriteConcern);

    jsTestLog("Attempting to unset the default write concern should fail.");
    assert.commandFailedWithCode(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {},
    }),
                                 ErrorCodes.IllegalOperation);

    res = conn.adminCommand({getDefaultRWConcern: 1});
    assert.eq(res.defaultWriteConcern, newDefaultWriteConcern);
}

const name = jsTestName();
const rst = new ReplSetTest({name: name, nodes: 2});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
runTest(primary);
rst.stopSet();

const st = new ShardingTest({name: name});
const mongos = st.s;
runTest(mongos);
st.stop();
})();
