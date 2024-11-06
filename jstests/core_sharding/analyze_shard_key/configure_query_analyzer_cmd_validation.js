/**
 * Tests basic validation within the configureQueryAnalyzer command.
 *
 * @tags: [requires_fcv_70]
 */
import {
    runInvalidNamespaceTestsForConfigure
} from "jstests/sharding/analyze_shard_key/libs/validation_common.js";

const dbName = jsTestName();
const mongos = db.getMongo();

runInvalidNamespaceTestsForConfigure(mongos, dbName);
