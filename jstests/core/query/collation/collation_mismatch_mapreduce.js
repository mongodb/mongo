/**
 * mapReduce should fail when source and destination have mismatched collations.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: mapReduce.
 *   not_allowed_with_signed_security_token,
 *   # TODO SERVER-117493 revisit enabling this in sharding passthroughs.
 *   assumes_against_mongod_not_mongos,
 *   # Timeseries collections cannot have unique indexes.
 *   exclude_from_timeseries_crud_passthrough,
 * ]
 */

const testDb = db.getSiblingDB(jsTestName());
const coll = testDb.collation_mapreduce4;
const outCollName = coll.getName() + "_outcoll";
const outColl = testDb[outCollName];
coll.drop();
outColl.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(testDb.createCollection(outColl.getName()));
try {
    coll.mapReduce(
        function () {
            emit(this.str, this.amt);
        },
        function (key, values) {
            return Array.sum(values);
        },
        {out: {reduce: outCollName}},
    );
    assert(false);
} catch (error) {
    assert.commandFailedWithCode(error, 51183);
}
