// Tests compute mode.
//
// @tags: [requires_sharding]
(function() {
'use strict';

const mongodOption = {
    setParameter: "enableComputeMode=true"
};

const shardsvrOption = {
    replSet: jsTestName(),
    shardsvr: "",
    setParameter: "enableComputeMode=true"
};

const configsvrOption = {
    replSet: jsTestName(),
    configsvr: "",
    setParameter: "enableComputeMode=true"
};

(function testComputeModeInStandaloneMongod() {
    const conn = MongoRunner.runMongod(mongodOption);

    const db = conn.getDB("admin");
    const startupLogResults = assert.commandWorked(db.adminCommand({getLog: "startupWarnings"}),
                                                   "ERROR from getting startupWarnings");
    assert.gt(startupLogResults.totalLinesWritten, 0, "ERROR from count of startup log message");

    let warningMsgFound = false;
    startupLogResults.log.forEach(log => {
        if (log.match("6968201")) {
            warningMsgFound = true;
        }
    });
    assert(warningMsgFound, "ERROR from warning message match");

    MongoRunner.stopMongod(conn);
})();

(function testComputeModeWithReplSet() {
    const rs = new ReplSetTest({nodes: 1});
    assert.throws(() => rs.startSet(mongodOption), [], "ERROR from replset test");
})();

(function testComputeModeWithShardedCluster() {
    const rs = new ReplSetTest({nodes: 1});
    // This emulates that mongod is running as a shard.
    assert.throws(() => rs.startSet(shardsvrOption), [], "ERROR from shardsvr test");
    // This emulates that mongod is running as a config server.
    assert.throws(() => rs.startSet(configsvrOption), [], "ERROR from configsvr test");
})();
}());
