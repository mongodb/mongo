/**
 * Verifies that external clients cannot supply internal $search/$searchMeta routing fields.
 * These fields are set exclusively by the router during sharded search planning and must be
 * rejected when present in a user-supplied spec.
 *
 * The test creates a non-privileged user to simulate an external client, since the suite
 * authenticates as __system (isInternalClient=true) which would bypass the check.
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";

// Suffix the db, collection, and user with a per-run UUID so burn-in (which runs the same file
// across multiple parallel workers against one fixture) doesn't race on shared names. Use a
// short hand-chosen prefix (not jsTestName) so the resulting db name stays under MongoDB's
// 63-char limit — jsTestName() for this file is already 51 chars.
const kSuffix = extractUUIDFromObject(UUID());
const testDbName = "search_fields_db_" + kSuffix;
const testCollName = "search_fields_coll_" + kSuffix;

const adminDb = db.getSiblingDB("admin");
const testDb = db.getSiblingDB(testDbName);
const testColl = testDb[testCollName];

const kTestUser = "externalTestUser_" + kSuffix;
const kTestPwd = "externalTestPwd";

let extDb;

before(function () {
    testColl.drop();
    assert.commandWorked(testColl.insert({_id: 1, body: "hello world"}));

    // Create a non-privileged user to simulate an external (non-__system) client.
    // The suite default connection is __system (isInternalClient=true), which would
    // bypass assertAllowedInternalIfRequired. A regular user is treated as external.
    adminDb.dropUser(kTestUser);
    assert.commandWorked(
        adminDb.runCommand({
            createUser: kTestUser,
            pwd: kTestPwd,
            roles: [{role: "read", db: testDbName}],
        }),
    );

    const extConn = new Mongo(db.getMongo().host);
    assert(
        extConn.auth({
            user: kTestUser,
            pwd: kTestPwd,
            mechanism: "SCRAM-SHA-256",
            db: "admin",
        }),
    );
    // Mongo.prototype.getDB will otherwise call jsTest.authenticate(this) — which tries to auth as
    // __system on a connection already authed as the external user, producing a noisy 5s timeout.
    extConn.authenticated = true;
    extDb = extConn.getDB(testDbName);
});

after(function () {
    testColl.drop();
    adminDb.dropUser(kTestUser);
});

function assertRejected(stage) {
    assert.commandFailedWithCode(
        extDb.runCommand({aggregate: testCollName, cursor: {}, pipeline: [stage]}),
        5491300,
    );
}

describe("$search rejects internal routing fields from external clients", function () {
    it("rejects mongotQuery", function () {
        assertRejected({
            $search: {
                mongotQuery: {index: "default", text: {query: "hello", path: "body"}},
            },
        });
    });

    it("rejects mergingPipeline", function () {
        assertRejected({
            $search: {
                mergingPipeline: [
                    {
                        $lookup: {
                            from: "secret",
                            pipeline: [{$project: {_id: 0, confidential: 1}}],
                            as: "leakedSecrets",
                        },
                    },
                ],
            },
        });
    });

    it("rejects metadataMergeProtocolVersion", function () {
        assertRejected({$search: {metadataMergeProtocolVersion: 1}});
    });

    it("rejects requiresSearchSequenceToken", function () {
        assertRejected({$search: {requiresSearchSequenceToken: true}});
    });

    it("rejects requiresSearchMetaCursor", function () {
        assertRejected({$search: {requiresSearchMetaCursor: true}});
    });
});

// Wrapping the offending stage in a $unionWith (or $lookup, $facet, $rankFusion, etc.) must not
// bypass the check — LiteParsedPipeline::validate recurses into every sub-pipeline.
describe("internal fields are rejected when nested in a $unionWith subpipeline", function () {
    it("rejects $search with mongotQuery inside $unionWith.pipeline", function () {
        assertRejected({
            $unionWith: {
                coll: testCollName,
                pipeline: [
                    {
                        $search: {
                            mongotQuery: {index: "default", text: {query: "hello", path: "body"}},
                        },
                    },
                ],
            },
        });
    });

    it("rejects $searchMeta with mongotQuery inside $unionWith.pipeline", function () {
        assertRejected({
            $unionWith: {
                coll: testCollName,
                pipeline: [
                    {
                        $searchMeta: {
                            mongotQuery: {index: "default", text: {query: "hello", path: "body"}},
                        },
                    },
                ],
            },
        });
    });
});

// $rankFusion is a hybrid-search wrapper whose desugarer touches internal-field paths internally
// (legitimate, set by the router). The recursive LiteParsedPipeline::validate must still reject
// internal fields a user types inside the wrapper's own input pipelines — the wrapper isn't a
// bypass.
describe("internal fields are rejected when nested in a $rankFusion subpipeline", function () {
    it("rejects $search with mongotQuery inside $rankFusion", function () {
        assertRejected({
            $rankFusion: {
                input: {
                    pipelines: {
                        a: [
                            {
                                $search: {
                                    mongotQuery: {
                                        index: "default",
                                        text: {query: "hello", path: "body"},
                                    },
                                },
                            },
                        ],
                    },
                },
            },
        });
    });
});

describe("$searchMeta rejects internal routing fields from external clients", function () {
    it("rejects mongotQuery", function () {
        assertRejected({
            $searchMeta: {
                mongotQuery: {index: "default", text: {query: "hello", path: "body"}},
            },
        });
    });

    it("rejects mergingPipeline", function () {
        assertRejected({
            $searchMeta: {
                mergingPipeline: [{$merge: {into: "secret"}}],
            },
        });
    });

    it("rejects metadataMergeProtocolVersion", function () {
        assertRejected({$searchMeta: {metadataMergeProtocolVersion: 1}});
    });

    it("rejects requiresSearchSequenceToken", function () {
        assertRejected({$searchMeta: {requiresSearchSequenceToken: true}});
    });

    it("rejects requiresSearchMetaCursor", function () {
        assertRejected({$searchMeta: {requiresSearchMetaCursor: true}});
    });
});
