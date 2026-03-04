/**
 * Verifies mongos can parse and propagate MultipleErrorsOccurred extra info received from a
 * shard.
 *
 * @tags: [
 *   requires_fcv_83,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const collName = "multiple_errors_occurred_info_parse";
const ns = `${dbName}.${collName}`;

const st = new ShardingTest({
    mongos: 1,
    shards: 1,
    rs: {nodes: 1},
});

const mongosDB = st.s.getDB(dbName);
const shardPrimary = st.rs0.getPrimary();

assert.commandWorked(mongosDB.runCommand({insert: collName, documents: [{_id: 0}]}));

assert.commandWorked(
    shardPrimary.adminCommand({
        configureFailPoint: "failCommand",
        mode: {times: 1},
        data: {
            errorCode: ErrorCodes.MultipleErrorsOccurred,
            failCommands: ["insert"],
            namespace: ns,
            failInternalCommands: true,
            errorExtraInfo: {
                causedBy: [
                    {index: 0, code: ErrorCodes.NamespaceNotFound, errmsg: "first error"},
                    {index: 0, code: ErrorCodes.StaleConfig, errmsg: "second error"},
                ],
            },
        },
    }),
);

const res = mongosDB.runCommand({insert: collName, documents: [{_id: 1}]});
assert(res.hasOwnProperty("writeErrors"), tojson(res));
assert.eq(res.writeErrors.length, 1, tojson(res));

const writeError = res.writeErrors[0];
assert(writeError.code == ErrorCodes.MultipleErrorsOccurred, tojson(res));

assert(!writeError.errmsg.includes("Error parsing extra info for MultipleErrorsOccurred"), tojson(res));
assert(writeError.hasOwnProperty("errInfo"), tojson(res));
assert(Array.isArray(writeError.errInfo.causedBy), tojson(res));
assert.eq(writeError.errInfo.causedBy.length, 2, tojson(res));
assert.eq(writeError.errInfo.causedBy[0].code, ErrorCodes.NamespaceNotFound, tojson(res));
assert.eq(writeError.errInfo.causedBy[0].errmsg, "first error", tojson(res));
assert.eq(writeError.errInfo.causedBy[1].code, ErrorCodes.StaleConfig, tojson(res));
assert.eq(writeError.errInfo.causedBy[1].errmsg, "second error", tojson(res));

st.stop();
