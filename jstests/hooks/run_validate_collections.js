// Runner for validateCollections that runs full validation on all collections when loaded into
// the mongo shell.
'use strict';

(function() {
load('jstests/libs/discover_topology.js');      // For Topology and DiscoverTopology.
load('jstests/hooks/validate_collections.js');  // For CollectionValidator.

assert.eq(typeof db, 'object', 'Invalid `db` object, is the shell connected to a mongod?');
const topology = DiscoverTopology.findConnectedNodes(db.getMongo());

const hostList = [];

if (topology.type === Topology.kStandalone) {
    hostList.push(topology.mongod);
} else if (topology.type === Topology.kReplicaSet) {
    hostList.push(...topology.nodes);
    new ReplSetTest(topology.nodes[0]).awaitSecondaryNodes();
} else if (topology.type === Topology.kShardedCluster) {
    hostList.push(...topology.configsvr.nodes);
    if (topology.configsvr.nodes.length > 1) {
        new ReplSetTest(topology.configsvr.nodes[0]).awaitSecondaryNodes();
    }

    for (let shardName of Object.keys(topology.shards)) {
        const shard = topology.shards[shardName];

        if (shard.type === Topology.kStandalone) {
            hostList.push(shard.mongod);
        } else if (shard.type === Topology.kReplicaSet) {
            hostList.push(...shard.nodes);
            new ReplSetTest(shard.nodes[0]).awaitSecondaryNodes();
        } else {
            throw new Error('Unrecognized topology format: ' + tojson(topology));
        }
    }
} else {
    throw new Error('Unrecognized topology format: ' + tojson(topology));
}

const adminDB = db.getSiblingDB('admin');
let requiredFCV = jsTest.options().forceValidationWithFeatureCompatibilityVersion;

let originalFCV;
let originalTransactionLifetimeLimitSeconds;

if (requiredFCV) {
    requiredFCV = new Function(
        `return typeof ${requiredFCV} === "string" ? ${requiredFCV} : "${requiredFCV}"`)();

    // Running the setFeatureCompatibilityVersion command may implicitly involve running a
    // multi-statement transaction. We temporarily raise the transactionLifetimeLimitSeconds to be
    // 24 hours to avoid spurious failures from it having been set to a lower value.
    originalTransactionLifetimeLimitSeconds = hostList.map(hostStr => {
        const conn = new Mongo(hostStr);
        const res = assert.commandWorked(
            conn.adminCommand({setParameter: 1, transactionLifetimeLimitSeconds: 24 * 60 * 60}));
        return {conn, originalValue: res.was};
    });

    originalFCV = adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});

    if (originalFCV.targetVersion) {
        let cmd = {setFeatureCompatibilityVersion: originalFCV.targetVersion};
        if (originalFCV.version === lastLTSFCV && originalFCV.targetVersion === lastContinuousFCV) {
            // We are only able to call 'setFeatureCompatibilityVersion' to transition from last-lts
            // to last-continuous with 'fromConfigServer: true'.
            cmd.fromConfigServer = true;
        }
        // If a previous FCV upgrade or downgrade was interrupted, then we run the
        // setFeatureCompatibilityVersion command to complete it before attempting to set the
        // feature compatibility version to 'requiredFCV'.
        assert.commandWorked(adminDB.runCommand(cmd));
        checkFCV(adminDB, originalFCV.targetVersion);
    }

    // Now that we are certain that an upgrade or downgrade of the FCV is not in progress, ensure
    // the 'requiredFCV' is set.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: requiredFCV}));
}

new CollectionValidator().validateNodes(hostList);

if (originalFCV && originalFCV.version !== requiredFCV) {
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: originalFCV.version}));
}

if (originalTransactionLifetimeLimitSeconds) {
    for (let {conn, originalValue} of originalTransactionLifetimeLimitSeconds) {
        assert.commandWorked(
            conn.adminCommand({setParameter: 1, transactionLifetimeLimitSeconds: originalValue}));
    }
}
})();
