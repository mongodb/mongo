/**
 * Tests that timeseries collections can be created in internal dabatabases
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

let internal_dbs = ["admin", "config"];
if (!FixtureHelpers.isMongos(db)) {
    // Can't use local DB through mongos
    internal_dbs.push("local");
}
const collName = `coll_${UUID()}`;

for (const dbName of internal_dbs) {
    assert.commandWorked(db.getSiblingDB(dbName).runCommand({create: collName, timeseries: {timeField: "time"}}));
}
