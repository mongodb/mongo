(function() {
'use strict';

load("jstests/libs/discover_topology.js");
load("jstests/sharding/libs/resharding_test_fixture.js");

const kNamespace = 'test.resharding';
function getCurrentOpSection(mongo, role) {
    let curOpSection = {};
    assert.soon(() => {
        const report = mongo.getDB("admin").currentOp(
            {ns: kNamespace, desc: {$regex: 'ReshardingMetrics' + role + 'Service'}});

        if (report.inprog.length === 1) {
            curOpSection = report.inprog[0];
            return true;
        }

        jsTest.log(tojson(report));
        return false;
    }, `: was unable to find resharding ${role} service in currentOp output from ${mongo.host}`);
    return curOpSection;
}

function getServerStatusSection(mongo) {
    const stats = mongo.getDB('admin').serverStatus({});
    assert(stats.hasOwnProperty('shardingStatistics'), stats);
    const shardingStats = stats.shardingStatistics;
    assert(shardingStats.hasOwnProperty('resharding'),
           `Missing resharding section in ${tojson(shardingStats)}`);

    return shardingStats.resharding;
}

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: true});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const inputCollection = reshardingTest.createShardedCollection({
    ns: kNamespace,
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});

const recipientShardNames = reshardingTest.recipientShardNames;
const topology = DiscoverTopology.findConnectedNodes(inputCollection.getMongo());
let allNodes = [];
for (let [_, shardReplSet] of Object.entries(topology.shards)) {
    allNodes.push(shardReplSet.primary);
}
allNodes.push(topology.configsvr.primary);
allNodes.forEach((hostName) => {
    const status = new Mongo(hostName).getDB('admin').serverStatus({});
    if (hostName == topology.configsvr.primary) {
        assert(!status.hasOwnProperty('shardingStatistics'));
        return;
    }
    const shardingStats = status.shardingStatistics;
    assert(!shardingStats.hasOwnProperty('resharding'));
});

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
            {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    (tempNs) => {
        // Wait for the resharding operation and the donor services to start.
        const mongos = inputCollection.getMongo();
        assert.soon(() => {
            const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                ns: inputCollection.getFullName()
            });
            return coordinatorDoc !== null && coordinatorDoc.cloneTimestamp !== undefined;
        });

        donorShardNames.forEach(function(shardName) {
            const curOpSection =
                getCurrentOpSection(new Mongo(topology.shards[shardName].primary), "Donor");
            assert(curOpSection.hasOwnProperty('donorState'), tojson(curOpSection));
            assert(curOpSection.hasOwnProperty('totalCriticalSectionTimeElapsedSecs'),
                   tojson(curOpSection));
        });

        recipientShardNames.forEach(function(shardName) {
            const curOpSection =
                getCurrentOpSection(new Mongo(topology.shards[shardName].primary), "Recipient");
            assert(curOpSection.hasOwnProperty('recipientState'), tojson(curOpSection));
            assert(curOpSection.hasOwnProperty('documentsCopied'), tojson(curOpSection));
            assert(curOpSection.hasOwnProperty('oplogEntriesApplied'), tojson(curOpSection));
        });

        const curOpSection =
            getCurrentOpSection(new Mongo(topology.configsvr.nodes[0]), "Coordinator");
        assert(curOpSection.hasOwnProperty('coordinatorState'), tojson(curOpSection));
        assert(curOpSection.hasOwnProperty('totalCriticalSectionTimeElapsedSecs'),
               tojson(curOpSection));
    });

allNodes.forEach((hostName) => {
    const serverStatus = getServerStatusSection(new Mongo(hostName));

    let debugStr = () => {
        return 'server: ' + tojson(hostName) + ', serverStatusSection: ' + tojson(serverStatus);
    };
    assert(serverStatus.hasOwnProperty('countSucceeded'), debugStr());
    assert(serverStatus.hasOwnProperty('countFailed'), debugStr());

    assert(serverStatus.hasOwnProperty('active'), debugStr());
    assert(serverStatus.active.hasOwnProperty('documentsCopied'), debugStr());

    assert(serverStatus.hasOwnProperty('oldestActive'), debugStr());
    assert(
        serverStatus.oldestActive.hasOwnProperty('recipientRemainingOperationTimeEstimatedMillis'),
        debugStr());

    assert(serverStatus.hasOwnProperty('latencies'), debugStr());
    assert(serverStatus.latencies.hasOwnProperty('collectionCloningTotalLocalInsertTimeMillis'),
           debugStr());

    assert(serverStatus.hasOwnProperty('currentInSteps'), debugStr());
    assert(serverStatus.currentInSteps.hasOwnProperty(
               'countInstancesInRecipientState2CreatingCollection'),
           debugStr());
});

reshardingTest.teardown();
})();
