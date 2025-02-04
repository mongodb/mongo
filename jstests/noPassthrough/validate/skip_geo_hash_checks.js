/**
 * As of SERVER-98427 we are skipping the hash checks for missing/extra keys for geo
 * indexes as floating point errors can cause spurious failures. This test validates
 * that they are skipped while also ensuring other valid failures are caught
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 * ]
 */
import {getUriForIndex, truncateUriAndRestartMongod} from "jstests/disk/libs/wt_file_helper.js";

const dbpath = MongoRunner.dataPath + 'skip_geo_hash_checks';
resetDbpath(dbpath);
let conn = MongoRunner.runMongod();
let db = conn.getCollection('test.skip_geo_hash');

assert.commandWorked(db.getDB().createCollection(db.getName()));
assert.commandWorked(db.createIndex({loc: '2dsphere'}));
assert.commandWorked(db.createIndex({loc: '2d'}));

assert.commandWorked(db.insertMany([
    {
        loc: [Math.random(), Math.random()],
    },
    {
        loc: [Math.random(), Math.random()],
    },
    {
        loc: [Math.random(), Math.random()],
    }
]));

let result = assert.commandWorked(db.validate());
jsTestLog(result);
assert(result.valid);

// Truncate the index but this will pass validation because we are no longer performing this
// specific check
const uri = getUriForIndex(db, "loc_2dsphere");
conn = truncateUriAndRestartMongod(uri, conn);
db = conn.getCollection("test.skip_geo_hash");
result = assert.commandWorked(db.validate());
jsTestLog(result);
assert(result.valid);

// Ensure that other index errors will fail for geo indexes
assert.commandWorked(
    conn.adminCommand({configureFailPoint: "failIndexKeyOrdering", mode: "alwaysOn"}));
result = assert.commandWorked(db.validate());
jsTestLog(result);
// Check this index specifically because loc2d_sphere has no keys to compare ordering, and _id will
// also cause result.valid to fail
assert(!result.indexDetails["loc_2d"].valid);
assert.commandWorked(conn.adminCommand({configureFailPoint: "failIndexKeyOrdering", mode: "off"}));

MongoRunner.stopMongod(conn, null, {skipValidation: true});
