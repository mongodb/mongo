/**
 * Verifies that external clients cannot supply internal $search/$searchMeta routing fields
 * (mongotQuery, mergingPipeline, metadataMergeProtocolVersion, requiresSearchSequenceToken,
 * requiresSearchMetaCursor). These fields are set exclusively by the router during sharded search
 * planning and must be rejected when present in a user-supplied spec.
 *
 * The test creates a non-privileged user to simulate an external client, since the suite
 * authenticates as __system (isInternalClient=true) which would bypass the check.
 */

const testDbName = jsTestName();
const testCollName = jsTestName();

const adminDb = db.getSiblingDB("admin");
const testDb = db.getSiblingDB(testDbName);
const testColl = testDb[testCollName];

const kTestUser = "externalTestUser";
const kTestPwd = "externalTestPwd";

testColl.drop();
assert.commandWorked(testColl.insert({_id: 1, body: "hello world"}));

// Create a non-privileged user to simulate an external (non-__system) client.
// The suite default connection is __system (isInternalClient=true), which would
// bypass assertAllowedInternalIfRequired. A regular user is treated as external.
adminDb.dropUser(kTestUser);
assert.commandWorked(adminDb.runCommand({
    createUser: kTestUser,
    pwd: kTestPwd,
    roles: [{role: "read", db: testDbName}],
}));

const extConn = new Mongo(db.getMongo().host);
assert(extConn.auth({
    user: kTestUser,
    pwd: kTestPwd,
    mechanism: "SCRAM-SHA-256",
    db: "admin",
}));
const extDb = extConn.getDB(testDbName);

function assertRejected(stage) {
    assert.commandFailedWithCode(
        extDb.runCommand({aggregate: testCollName, cursor: {}, pipeline: [stage]}),
        5491300,
        "Expected stage to be rejected as setting an internal field from an external client: " +
            tojson(stage));
}

// $search rejects internal routing fields from external clients.
assertRejected({
    $search: {mongotQuery: {index: "default", text: {query: "hello", path: "body"}}},
});
assertRejected({
    $search: {
        mergingPipeline: [{
            $lookup: {
                from: "secret",
                pipeline: [{$project: {_id: 0, confidential: 1}}],
                as: "leakedSecrets",
            },
        }],
    },
});
assertRejected({$search: {metadataMergeProtocolVersion: 1}});
assertRejected({$search: {requiresSearchSequenceToken: true}});
assertRejected({$search: {requiresSearchMetaCursor: true}});
assertRejected({$search: {limit: 100}});
assertRejected({$search: {sortSpec: {field: 1}}});
assertRejected({$search: {mongotDocsRequested: 50}});

// $searchMeta rejects internal routing fields from external clients.
assertRejected({
    $searchMeta: {mongotQuery: {index: "default", text: {query: "hello", path: "body"}}},
});
assertRejected({$searchMeta: {mergingPipeline: [{$merge: {into: "secret"}}]}});
assertRejected({$searchMeta: {metadataMergeProtocolVersion: 1}});
assertRejected({$searchMeta: {requiresSearchSequenceToken: true}});
assertRejected({$searchMeta: {requiresSearchMetaCursor: true}});
assertRejected({$searchMeta: {limit: 100}});
assertRejected({$searchMeta: {sortSpec: {field: 1}}});
assertRejected({$searchMeta: {mongotDocsRequested: 50}});

// Wrapping the offending stage in a $unionWith subpipeline must not bypass the check —
// Pipeline::parse recurses through the inner pipeline, which calls each stage's createFromBson.
assertRejected({
    $unionWith: {
        coll: testCollName,
        pipeline: [
            {$search: {mongotQuery: {index: "default", text: {query: "hello", path: "body"}}}},
        ],
    },
});
assertRejected({
    $unionWith: {
        coll: testCollName,
        pipeline: [
            {$searchMeta: {mongotQuery: {index: "default", text: {query: "hello", path: "body"}}}},
        ],
    },
});

testColl.drop();
adminDb.dropUser(kTestUser);
