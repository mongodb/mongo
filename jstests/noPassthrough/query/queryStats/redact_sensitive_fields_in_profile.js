/**
 * Test that the queryStats HMAC key is not leaked during profiling.
 * @tags: [requires_fcv_71]
 */
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {getQueryStatsFindCmd} from "jstests/libs/query_stats_utils.js";

const conn = MongoRunner.runMongod({setParameter: {internalQueryStatsRateLimit: -1}, profile: 2});
const adminDB = conn.getDB("admin");
const testDB = conn.getDB("test");
const coll = testDB[jsTestName()];

// Prepopulate
coll.drop();
assert.commandWorked(coll.insert([{foo: 0}, {foo: 1}]));

const nsProfilerFilter = {
    ns: coll.getFullName()
};

// Run a few find commands
for (const index of Array(10).keys()) {
    if (index < 2) {
        assert.neq(coll.findOne({foo: index}), null, `{foo: ${index}} should be extant`);
    } else {
        assert.isnull(coll.findOne({foo: index}), `{foo: ${index}} should be null`);
    }
}

// This returns `null` when there are no entries, as opposed to erroring when getLatestProfilerEntry
// encounters an empty profile.
const getLastAdminEntry = () => {
    const cursor =
        adminDB.system.profile.find({ns: "admin.$cmd.aggregate"}).sort({$natural: -1}).limit(1);
    return [...cursor.toArray(), null][0];
};

const lastTestEntry = getLatestProfilerEntry(testDB, nsProfilerFilter);
const lastAdminEntry = getLastAdminEntry();
const hmacKey = BinData(8, "YW4gYXJiaXRyYXJ5IEhNQUNrZXkgZm9yIHRlc3Rpbmc=");

getQueryStatsFindCmd(conn, {transformIdentifiers: true, hmacKey: hmacKey});

// Check that the queryStats command is not profiled.
assert.eq(getLatestProfilerEntry(testDB, nsProfilerFilter), lastTestEntry);

// Check that the queryStats command is profiled in the admin log either.
const adminEntry = getLastAdminEntry();
assert.neq(adminEntry, lastAdminEntry);

// Check that the HMAC key is redacted.
const loggedHmacKey = adminEntry.command.pipeline[0].$queryStats.transformIdentifiers.hmacKey;
assert.neq(loggedHmacKey, hmacKey);

// This is somewhat implementation dependent. If the redaction string ever changes, this will need
// to be updated as well.
assert.eq(loggedHmacKey, "###");

MongoRunner.stopMongod(conn);
