// Runner for validateCollections that runs full validation on all collections when loaded into
// the mongo shell.
'use strict';

(function() {
load('jstests/libs/discover_topology.js');      // For Topology and DiscoverTopology.
load('jstests/libs/with_fcv.js');               // For withFCV.
load('jstests/hooks/validate_collections.js');  // For CollectionValidator.

assert.eq(typeof db, 'object', 'Invalid `db` object, is the shell connected to a mongod?');
const topology = DiscoverTopology.findConnectedNodes(db.getMongo());

const hostList = [];
let configPrimary;

if (topology.type === Topology.kStandalone) {
    hostList.push(topology.mongod);
} else if (topology.type === Topology.kReplicaSet) {
    hostList.push(...topology.nodes);
} else if (topology.type === Topology.kShardedCluster) {
    hostList.push(...topology.configsvr.nodes);

    for (let shardName of Object.keys(topology.shards)) {
        const shard = topology.shards[shardName];

        if (shard.type === Topology.kStandalone) {
            hostList.push(shard.mongod);
        } else if (shard.type === Topology.kReplicaSet) {
            hostList.push(...shard.nodes);
        } else {
            throw new Error('Unrecognized topology format: ' + tojson(topology));
        }
    }

    configPrimary = new Mongo(topology.configsvr.primary);
} else {
    throw new Error('Unrecognized topology format: ' + tojson(topology));
}

// Set the fail point on config server to allow FCV downgrade even in the presence of a
// collection sharded on a compound hashed key.
if (configPrimary) {
    assert.commandWorked(configPrimary.getDB('admin').runCommand(
        {configureFailPoint: 'allowFCVDowngradeWithCompoundHashedShardKey', mode: 'alwaysOn'}));
}

let requiredFCV = jsTest.options().forceValidationWithFeatureCompatibilityVersion;
if (requiredFCV) {
    requiredFCV = new Function(
        `return typeof ${requiredFCV} === "string" ? ${requiredFCV} : "${requiredFCV}"`)();
}

const maybeWithRequiredFCV = (callback) => {
    if (requiredFCV) {
        withFCV(db.getMongo(), requiredFCV, callback);
    } else {
        callback();
    }
};

maybeWithRequiredFCV(() => new CollectionValidator().validateNodes(hostList));

// Disable the failpoint that was set earlier on sharded clusters.
if (configPrimary) {
    assert.commandWorked(configPrimary.getDB('admin').runCommand(
        {configureFailPoint: 'allowFCVDowngradeWithCompoundHashedShardKey', mode: 'off'}));
}
})();
