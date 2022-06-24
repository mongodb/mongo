// Test that ClusterServerParameterOpObserver fires appropriately.
// @tags: [requires_replication]

(function() {
'use strict';

const kUnknownCSPLogId = 6226300;
const kUnknownCSPLogComponent = 'control';
const kUnknownCSPLogLevel = 3;

function runTest(conn) {
    const config = conn.getDB('config');
    const originalLogLevel =
        assert.commandWorked(config.setLogLevel(kUnknownCSPLogLevel, kUnknownCSPLogComponent))
            .was.verbosity;
    assert.writeOK(
        config.clusterParameters.insert({_id: 'foo', clusterParameterTime: Date(), value: 123}));
    assert.commandWorked(config.setLogLevel(originalLogLevel, kUnknownCSPLogComponent));
    assert(checkLog.checkContainsOnceJson(conn, kUnknownCSPLogId, {name: 'foo'}));
}

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
runTest(rst.getPrimary());
rst.stopSet();
})();
