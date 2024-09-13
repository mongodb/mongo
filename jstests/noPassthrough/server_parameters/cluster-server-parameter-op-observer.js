// Test that ClusterServerParameterOpObserver fires appropriately.
// @tags: [requires_replication, requires_fcv_71]

import {ReplSetTest} from "jstests/libs/replsettest.js";

function runTest(conn) {
    const config = conn.getDB('config');
    const res =
        config.clusterParameters.insert({_id: 'foo', clusterParameterTime: Date(), value: 123});
    assert(res.hasWriteError());
    assert.neq(res.getWriteError().length, 0);
}

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
runTest(rst.getPrimary());
rst.stopSet();