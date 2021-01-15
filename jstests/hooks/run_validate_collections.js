// Runner for validateCollections that runs full validation on all collections when loaded into
// the mongo shell.
'use strict';

(function() {
load('jstests/libs/discover_topology.js');      // For Topology and DiscoverTopology.
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

const adminDB = db.getSiblingDB('admin');
let requiredFCV = jsTest.options().forceValidationWithFeatureCompatibilityVersion;

let originalFCV;
let originalTransactionLifetimeLimitSeconds;

let skipFCV = false;

if (requiredFCV) {
    // Can't set the FCV to 4.2 while having long collection names present.
    adminDB.runCommand("listDatabases").databases.forEach(function(d) {
        const mdb = adminDB.getSiblingDB(d.name);
        try {
            mdb.getCollectionInfos().forEach(function(c) {
                const namespace = d.name + "." + c.name;
                const namespaceLenBytes = encodeURIComponent(namespace).length;
                if (namespaceLenBytes > 120) {
                    skipFCV = true;
                }
            });
        } catch (e) {
            skipFCV = true;
        }
    });

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
        // If a previous FCV upgrade or downgrade was interrupted, then we run the
        // setFeatureCompatibilityVersion command to complete it before attempting to set the
        // feature compatibility version to 'requiredFCV'.
        assert.commandWorked(
            adminDB.runCommand({setFeatureCompatibilityVersion: originalFCV.targetVersion}));
        checkFCV(adminDB, originalFCV.targetVersion);
    }

    // Now that we are certain that an upgrade or downgrade of the FCV is not in progress, ensure
    // the 'requiredFCV' is set.
    if (!skipFCV) {
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: requiredFCV}));
    }
}

new CollectionValidator().validateNodes(hostList, skipFCV);

if (originalFCV && originalFCV.version !== requiredFCV && !skipFCV) {
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: originalFCV.version}));
}

if (originalTransactionLifetimeLimitSeconds) {
    for (let {conn, originalValue} of originalTransactionLifetimeLimitSeconds) {
        assert.commandWorked(
            conn.adminCommand({setParameter: 1, transactionLifetimeLimitSeconds: originalValue}));
    }
}

// Disable the failpoint that was set earlier on sharded clusters.
if (configPrimary) {
    assert.commandWorked(configPrimary.getDB('admin').runCommand(
        {configureFailPoint: 'allowFCVDowngradeWithCompoundHashedShardKey', mode: 'off'}));
}
})();
