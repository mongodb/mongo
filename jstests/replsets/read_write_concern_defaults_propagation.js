// Tests propagation of RWC defaults across a replica set.
import {
    ReadWriteConcernDefaultsPropagation
} from "jstests/libs/read_write_concern_defaults_propagation_common.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondaries = rst.getSecondaries();

ReadWriteConcernDefaultsPropagation.runTests(primary, [primary, ...secondaries]);

// Verify the in-memory defaults are updated correctly. This verifies the cache is invalidated
// properly on secondaries when an update to the defaults document is replicated because the
// in-memory value will only be updated after an invalidation.
ReadWriteConcernDefaultsPropagation.runTests(
    primary, [primary, ...secondaries], true /* inMemory */);

ReadWriteConcernDefaultsPropagation.runDropAndDeleteTests(primary, [primary, ...secondaries]);

rst.stopSet();