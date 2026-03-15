import {Mongorestore} from "jstests/libs/mongodb_database_tools.js";
import {checkPauseAfterPopulate} from "jstests/libs/pause_after_populate.js";

/**
 * Populate the TPCH dataset by restoring it from a mongorestore archive. This requires the following prerequisites:
 * - the `mongorestore` tool, accessible on the $PATH.
 * - the TPC-H dataset, located in a directory named `tpc-h` that is on the same level as the mongodb repository.
 *
 * `mongorestore` is part of the "MongoDB Database Tools" package,
 * available at https://www.mongodb.com/try/download/database-tools
 *
 * The TPC-H dataset is available from the `query-benchmark-data` S3 bucket.
 *
 * In evergreen, tasks such as `query_golden_join_optimization_plan_stability`
 * make sure the prerequisites are already in place.
 */
export function populateTPCHDataset(scale) {
    const mr = new Mongorestore();
    const dbName = jsTestName();

    mr.execute({
        archive: `../tpc-h/tpch-${scale}-normalized.archive.gz`,
        nsFrom: "tpch.*",
        nsTo: `${dbName}.*`,
        drop: true,
        maintainInsertionOrder: true,
        gzip: true,
    });

    checkPauseAfterPopulate();

    return db.getMongo().getDB(dbName);
}
