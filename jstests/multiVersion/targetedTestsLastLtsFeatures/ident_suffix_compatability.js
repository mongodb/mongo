/**
 * Tests collection contents are preserved across upgrade/downgrade when ident generation changes
 * across versions.
 */
import "jstests/multiVersion/libs/verify_versions.js";
import "jstests/multiVersion/libs/multi_rs.js";

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const lastLTSVersion = {
    binVersion: "last-lts"
};
const latestVersion = {
    binVersion: "latest"
};

const dbName = jsTestName();
// The set of documents expected for each collection in 'dbName'.
const docs = [{_id: 0, x: 0}, {_id: 1, x: 1}];
const getDB = (primaryConnection) => primaryConnection.getDB(dbName);

// Prints collection idents for collections in the 'dbName' database.
function printIdents(node) {
    const identsResult = node.getDB('admin')
                             .aggregate([
                                 {$listCatalog: {}},
                                 {$match: {db: dbName}},
                                 {$project: {ns: 1, ident: 1, idxIdent: 1}}
                             ])
                             .toArray();
    jsTestLog(`Collection idents on ${node.port}: ${tojson(identsResult)}`);
}

// Given a list of 'collNames', asserts the corresponding collection exists and contains 'docs' on
// each node of the replica set.
function validateCollectionContents(rst, collNames) {
    rst.awaitReplication();
    rst.nodes.forEach((node) => {
        printIdents(node, dbName);
        for (let collName of collNames) {
            const coll = getDB(node)[collName];
            const foundDocs = coll.find().toArray();
            assert.sameMembers(
                docs,
                foundDocs,
                `Expected collection ${collName} on ${node.host} to contain all docs: ${
                    tojson(docs)}. Found docs: ${tojson(foundDocs)}`);
        }
    });
    rst.checkReplicatedDataHashes();
}

function setupCollection(rst, collName) {
    const primaryConnection = rst.getPrimary();
    const coll = assertDropAndRecreateCollection(getDB(primaryConnection), collName);
    assert.commandWorked(coll.insertMany(docs));
}

function downgradeSecondariesToLastLTS(rst) {
    // First downgrade FCV to simulate replica set downgrade.
    const primaryConnection = rst.getPrimary();
    assert.commandWorked(primaryConnection.adminCommand(
        {setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

    // Finally, downgrade the secondaries to result in a mixed bin version replica set.
    rst.upgradeSecondaries({...lastLTSVersion});
}

// Starting 8.2, ident suffix generation changes to encode a UUID rather than a random number +
// counter. Tests that idents generated in newer versions are still compatible with 8.0.
function testIdentsCompatibleAcrossVersions() {
    const rst = new ReplSetTest({
        name: jsTestName(),
        nodes: [
            {...latestVersion},
            {...latestVersion},
        ],
    });
    rst.startSet();
    rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

    jsTestLog(`Creating new collection when all nodes are on ${tojson(latestVersion)}`);
    const collLatestName = "collCreatedOnLatest";
    setupCollection(rst, collLatestName);
    validateCollectionContents(rst, [collLatestName]);

    jsTestLog(`Downgrading secondaries to simulate mixed version replica set`);
    downgradeSecondariesToLastLTS(rst);
    assert.binVersion(rst.getPrimary(), latestVersion.binVersion);
    assert.binVersion(rst.getSecondary(), lastLTSVersion.binVersion);

    jsTestLog(`Creating collection with on replica set with mixed binVersions`);
    const collMixedName = "collCreatedOnMixedVersionReplica";
    setupCollection(rst, collMixedName);

    jsTestLog(`Validating collection contents in the mixed version replica set`);
    validateCollectionContents(rst, [collLatestName, collMixedName]);

    jsTestLog(`Downgrading entire set to lastLTS`);
    rst.upgradeSet({...lastLTSVersion});
    assert.binVersion(rst.getPrimary(), lastLTSVersion.binVersion);

    // The new node is on the older version, and generates idents, for both
    // collections, using the legacy method.
    jsTestLog(`Initial syncing a new node on lastLTS`);
    rst.add({...lastLTSVersion});
    rst.reInitiate();
    rst.awaitReplication();
    rst.awaitSecondaryNodes();

    jsTestLog(`Validating collection contents with all nodes on 'lastLTS'`);
    validateCollectionContents(rst, [collLatestName, collMixedName]);

    jsTestLog(`Finally, upgrading the entire set back to 'latest'`);
    rst.upgradeSet({...latestVersion});
    validateCollectionContents(rst, [collLatestName, collMixedName]);

    rst.stopSet();
}

testIdentsCompatibleAcrossVersions();
