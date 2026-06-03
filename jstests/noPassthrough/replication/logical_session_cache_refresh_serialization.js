/**
 * Test to surface issues with the semantics of the refreshLogicalSessionCacheNow command.
 * Using a replset with a short logical session refresh interval, run the command several times to make sure that sessions are inserted when it returns.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {LogicalSessionCacheRefreshSerializationTest} from "jstests/noPassthrough/replication/libs/logical_session_cache_refresh_serialization_test.js";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // Set short interval on logical session refresh
            logicalSessionRefreshMillis: 100,
            disableLogicalSessionCacheRefresh: false,
        },
    },
});
rst.startSet();
rst.initiate();

LogicalSessionCacheRefreshSerializationTest.run(rst.getPrimary());

rst.stopSet();
