/**
 * This file tests that if a user initiates a write that becomes a noop either due to being a
 * duplicate operation or due to errors relying on data reads, that we still wait for write concern.
 * This is because we must wait for write concern on the write that made this a noop so that we can
 * be sure it doesn't get rolled back if we acknowledge it.
 *
 * @tags: [
 * multiversion_incompatible,
 * uses_transactions,
 * does_not_support_stepdowns,
 * ]
 */

import {
    checkWriteConcernBehaviorAdditionalCRUDOps,
    checkWriteConcernBehaviorForAllCommands
} from "jstests/libs/write_concern_all_commands.js";

const name = jsTestName();
const replTest = new ReplSetTest({
    name: name,
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
});
replTest.startSet();
replTest.initiateWithHighElectionTimeout();

checkWriteConcernBehaviorForAllCommands(replTest.getPrimary(), replTest, "rs" /* clusterType */);
checkWriteConcernBehaviorAdditionalCRUDOps(replTest.getPrimary(), replTest, "rs" /* clusterType */);

replTest.stopSet();
