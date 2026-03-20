/**
 * Tests that timeseries collections can be created in internal dabatabases
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {
    isViewfulTimeseriesOnlySuite,
    isViewlessTimeseriesOnlySuite,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

let internal_dbs = ["admin", "config"];
if (!FixtureHelpers.isMongos(db)) {
    // Can't use local DB through mongos
    internal_dbs.push("local");
}

// TODO(SERVER-122270): Remove once _mdb_catalog and local.system.views are recovered consistently.
if (!isViewfulTimeseriesOnlySuite(db) && !isViewlessTimeseriesOnlySuite(db)) {
    internal_dbs = internal_dbs.filter((dbName) => dbName !== "local");
}

const collName = `coll_${UUID()}`;

for (const dbName of internal_dbs) {
    assert.commandWorked(db.getSiblingDB(dbName).runCommand({create: collName, timeseries: {timeField: "time"}}));
}
