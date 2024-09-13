/**
 * Tests setting CWWC with missing 'w' field should fail.
// @tags: [
//   requires_persistence,
//   requires_replication,
// ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runTest(conn) {
    let checkDefaultWC = (wc) => {
        let res = conn.adminCommand({getDefaultRWConcern: 1});
        assert(res.hasOwnProperty("defaultWriteConcern"));
        assert.eq(res.defaultWriteConcern, wc);
    };

    // Implicit default write concern is returned.
    checkDefaultWC({"w": "majority", "wtimeout": 0});
    jsTestLog("Setting only the default read concern works.");
    assert.commandWorked(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultReadConcern: {level: 'local'},
    }));
    // CWWC shouldn't change.
    checkDefaultWC({"w": "majority", "wtimeout": 0});

    jsTestLog("Sending empty wc to unset the default write concern initially works.");
    assert.commandWorked(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {},
    }));
    // CWWC shouldn't change.
    checkDefaultWC({"w": "majority", "wtimeout": 0});

    jsTestLog("Setting only 'wtimeout', command should fail with 'BadValue'.");
    assert.commandFailedWithCode(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {wtimeout: 60},
    }),
                                 ErrorCodes.BadValue);
    // CWWC shouldn't change.
    checkDefaultWC({"w": "majority", "wtimeout": 0});

    jsTestLog("Setting only 'j' field, command should fail with 'BadValue'.");
    assert.commandFailedWithCode(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {j: true},
    }),
                                 ErrorCodes.BadValue);
    // CWWC shouldn't change.
    checkDefaultWC({"w": "majority", "wtimeout": 0});

    jsTestLog("Setting only 'w' field succeeds.");
    assert.commandWorked(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {w: 1},
    }));
    checkDefaultWC({"w": 1, "wtimeout": 0});
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
