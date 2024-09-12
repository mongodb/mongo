/**
 * Test to ensure that if there are already 7 voting members in a set, adding a new voting member
 * will fail during validation.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "testdb";
const collName = "testcoll";

const rst = new ReplSetTest({name: jsTestName(), nodes: 7});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const primary = rst.getPrimary();
const primaryDb = primary.getDB(dbName);
const primaryColl = primaryDb.getCollection(collName);

assert.commandWorked(primaryColl.insert({x: 1, y: 2}));

const newNode = rst.add({});

const config = rst.getReplSetConfigFromNode();
const newConfig = rst.getReplSetConfig();

config.members = newConfig.members;
config.version += 1;

assert.commandFailedWithCode(primary.adminCommand({replSetReconfig: config}),
                             ErrorCodes.NewReplicaSetConfigurationIncompatible);

rst.remove(newNode);
rst.stopSet();
