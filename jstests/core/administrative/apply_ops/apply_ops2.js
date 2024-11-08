/*
 * Test applyops upsert flag SERVER-7452
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: applyOps.
 *   not_allowed_with_signed_security_token,
 *   requires_non_retryable_commands,
 *   requires_fastcount,
 *   # 6.2 removes support for atomic applyOps
 *   requires_fcv_62,
 *   # applyOps is not supported on mongos
 *   assumes_against_mongod_not_mongos,
 *   # applyOps uses the oplog that require replication support
 *   requires_replication,
 *   multiversion_incompatible
 * ]
 */

var t = db.apply_ops2;
t.drop();

assert.eq(0, t.find().count(), "test collection not empty");

t.insert({_id: 1, x: "init"});

print("Testing applyOps with alwaysUpsert explicity set to true");

var res = db.runCommand({
    applyOps: [
        {
            op: "u",
            ns: t.getFullName(),
            o2: {_id: 1},
            o: {$v: 2, diff: {u: {x: "upsert=true existing"}}}
        },
        {
            op: "u",
            ns: t.getFullName(),
            o2: {_id: 2},
            o: {$v: 2, diff: {u: {x: "upsert=true non-existing"}}}
        }
    ],
    alwaysUpsert: true
});

// Verify that the 'alwaysUpsert' option is no longer supported.
assert.commandFailedWithCode(res, 6711601);
assert.eq(
    1, t.find().count(), "1 doc expected after unsupported option failure on applyOps command");

// alwaysUpsert not specified, should default to false
print("Testing applyOps with default alwaysUpsert");

res = db.runCommand({
    applyOps: [
        {
            op: "u",
            ns: t.getFullName(),
            o2: {_id: 1},
            o: {$v: 2, diff: {u: {x: "upsert=false existing"}}}
        },
        {
            op: "u",
            ns: t.getFullName(),
            o2: {_id: 3},
            o: {$v: 2, diff: {u: {x: "upsert=false non-existing"}}}
        }
    ]
});

assert.eq(true, res.results[0], "upsert = true, existing doc expected to succeed, but it failed");
assert.eq(false, res.results[1], "upsert = false, nonexisting doc upserted");
assert.eq(1, t.find().count(), "1 doc expected after upsert failure");
