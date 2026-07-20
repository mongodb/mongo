/**
 * Verifies the external-client trust boundary for extension-provided $search / $searchMeta.
 *
 * Internal $search/$searchMeta routing fields (e.g. mongotQuery, mergingPipeline,
 * metadataMergeProtocolVersion, requiresSearchSequenceToken, requiresSearchMetaCursor) are set
 * exclusively by the router during sharded search planning. A server-implemented ("legacy")
 * $search rejects them outright (error code 5491300) when they are supplied by an external client.
 *
 * On the search-EXTENSION path there is no such rejection: the extension does not reject the spec
 * with 5491300 but instead folds the user-supplied fields into the mongot query (which typically
 * then fails). Rather than assert a specific error code, this test asserts the security invariant:
 * a user-supplied internal field must never cause confidential data to leak and must never take
 * effect as routing / merge logic. In particular, an injected mergingPipeline must not execute on
 * mongos with router privileges. A failed command (no data returned) is an acceptable, safe
 * outcome; a successful command must simply never contain confidential data.
 *
 * TODO SERVER-131798: once the extension enforces internal-field rejection itself (via a
 * client-type-aware Extensions-API validate() entrypoint), this test can assert an explicit
 * rejection instead of the weaker no-leak invariant.
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";

// Suffix the db, collection, user, and role with a per-run UUID so burn-in (which runs the same
// file across multiple parallel workers against one fixture) doesn't race on shared names. Use a
// short hand-chosen prefix (not jsTestName) so the resulting db name stays under MongoDB's 63-char
// limit.
const kSuffix = extractUUIDFromObject(UUID());
const testDbName = "search_ext_db_" + kSuffix;
const mainCollName = "search_ext_main_" + kSuffix;
const secretCollName = "search_ext_secret_" + kSuffix;
const indexName = "search_ext_idx_" + kSuffix;

const kTestUser = "extUser_" + kSuffix;
const kTestPwd = "externalTestPwd";
const kTestRole = "extRole_" + kSuffix;

// Sentinel value stored only in the secret collection. If it ever appears in the output of a query
// issued by the non-privileged external user, a user-supplied internal field caused a leak.
const kSecretSentinel = "LEAKED_CONFIDENTIAL_" + kSuffix;

const adminDb = db.getSiblingDB("admin");
const testDb = db.getSiblingDB(testDbName);
const mainColl = testDb[mainCollName];
const secretColl = testDb[secretCollName];

let extDb;

before(function () {
    mainColl.drop();
    secretColl.drop();

    assert.commandWorked(mainColl.insert({_id: 1, body: "hello world"}));
    assert.commandWorked(mainColl.insert({_id: 2, body: "hello there"}));

    // The secret collection holds a confidential sentinel doc. The external user has NO privilege
    // to read it — the only way its contents could surface is via an injected mergingPipeline that
    // runs with router privileges.
    assert.commandWorked(secretColl.insert({_id: 1, confidential: kSecretSentinel}));

    // Search index on the main collection over the `body` field.
    createSearchIndex(mainColl, {
        name: indexName,
        definition: {mappings: {dynamic: false, fields: {body: {type: "string"}}}},
    });

    // Create a custom role granting `find` ONLY on the main collection — importantly NOT on the
    // secret collection — then create the external user with only that role. The suite default
    // connection is __system (isInternalClient=true), which would bypass the external-client
    // boundary, so injected queries must run over a separate connection authed as this user.
    // dropRole/dropUser no-op (return false) when the role/user does not exist, so no guard needed.
    adminDb.dropRole(kTestRole);
    adminDb.dropUser(kTestUser);

    assert.commandWorked(
        adminDb.runCommand({
            createRole: kTestRole,
            privileges: [
                {
                    resource: {db: testDbName, collection: mainCollName},
                    actions: ["find"],
                },
            ],
            roles: [],
        }),
    );
    assert.commandWorked(
        adminDb.runCommand({
            createUser: kTestUser,
            pwd: kTestPwd,
            roles: [{role: kTestRole, db: "admin"}],
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
    dropSearchIndex(mainColl, {name: indexName});
    mainColl.drop();
    secretColl.drop();
    adminDb.dropUser(kTestUser);
    adminDb.dropRole(kTestRole);
});

// Security invariant: a user-supplied internal field must never leak confidential data or take
// effect. A failed command is a safe outcome (no data returned); a successful one must not contain
// the secret sentinel. A failure is in fact common: the injected field gets folded into the mongot
// query rather than rejected, usually producing a malformed query that mongot rejects. We tolerate
// res.ok === 0 rather than asserting it because the exact outcome varies by field.
function assertNoLeak(pipeline) {
    const res = extDb.runCommand({aggregate: mainCollName, cursor: {}, pipeline: pipeline});
    if (res.ok) {
        const docs = new DBCommandCursor(extDb, res).toArray();
        const dumped = tojson(docs);
        assert(
            !dumped.includes(kSecretSentinel),
            "internal-field injection leaked confidential data via pipeline " +
                tojson(pipeline) +
                " -> " +
                dumped,
        );
    }
    // res.ok === 0 (command failed) is an acceptable, safe outcome.
}

describe("$search internal-field-only injections remain inert on the extension path", function () {
    it("does not leak via mongotQuery", function () {
        assertNoLeak([
            {$search: {mongotQuery: {index: indexName, text: {query: "hello", path: "body"}}}},
        ]);
    });

    it("does not leak via mergingPipeline", function () {
        assertNoLeak([
            {
                $search: {
                    mergingPipeline: [
                        {
                            $lookup: {
                                from: secretCollName,
                                pipeline: [{$project: {_id: 0, confidential: 1}}],
                                as: "leaked",
                            },
                        },
                    ],
                },
            },
        ]);
    });

    it("does not leak via metadataMergeProtocolVersion", function () {
        assertNoLeak([{$search: {metadataMergeProtocolVersion: 1}}]);
    });

    it("does not leak via requiresSearchSequenceToken", function () {
        assertNoLeak([{$search: {requiresSearchSequenceToken: true}}]);
    });

    it("does not leak via requiresSearchMetaCursor", function () {
        assertNoLeak([{$search: {requiresSearchMetaCursor: true}}]);
    });
});

describe("$searchMeta internal-field-only injections remain inert on the extension path", function () {
    it("does not leak via mongotQuery", function () {
        assertNoLeak([
            {$searchMeta: {mongotQuery: {index: indexName, text: {query: "hello", path: "body"}}}},
        ]);
    });

    it("does not leak via mergingPipeline", function () {
        assertNoLeak([
            {
                $searchMeta: {
                    mergingPipeline: [
                        {
                            $lookup: {
                                from: secretCollName,
                                pipeline: [{$project: {_id: 0, confidential: 1}}],
                                as: "leaked",
                            },
                        },
                    ],
                },
            },
        ]);
    });

    it("does not leak via metadataMergeProtocolVersion", function () {
        assertNoLeak([{$searchMeta: {metadataMergeProtocolVersion: 1}}]);
    });

    it("does not leak via requiresSearchSequenceToken", function () {
        assertNoLeak([{$searchMeta: {requiresSearchSequenceToken: true}}]);
    });

    it("does not leak via requiresSearchMetaCursor", function () {
        assertNoLeak([{$searchMeta: {requiresSearchMetaCursor: true}}]);
    });
});

// Wrapping the offending stage in a $unionWith or $rankFusion input pipeline must not bypass the
// invariant — nested internal fields must remain equally inert.
describe("nested internal-field injections must not bypass the invariant", function () {
    it("does not leak via $search inside a $unionWith subpipeline", function () {
        assertNoLeak([
            {
                $unionWith: {
                    coll: mainCollName,
                    pipeline: [
                        {
                            $search: {
                                mongotQuery: {
                                    index: indexName,
                                    text: {query: "hello", path: "body"},
                                },
                            },
                        },
                    ],
                },
            },
        ]);
    });

    it("does not leak via $search inside a $rankFusion input pipeline", function () {
        assertNoLeak([
            {
                $rankFusion: {
                    input: {
                        pipelines: {
                            a: [
                                {
                                    $search: {
                                        mongotQuery: {
                                            index: indexName,
                                            text: {query: "hello", path: "body"},
                                        },
                                    },
                                },
                            ],
                        },
                    },
                },
            },
        ]);
    });
});

// This proves the user's mergingPipeline does not execute on mongos with router privileges and leak
// the secret collection.
describe("valid operator plus injected mergingPipeline must not leak", function () {
    it("does not leak the secret collection via an injected mergingPipeline", function () {
        assertNoLeak([
            {
                $search: {
                    index: indexName,
                    text: {query: "hello", path: "body"},
                    mergingPipeline: [
                        {
                            $lookup: {
                                from: secretCollName,
                                pipeline: [{$project: {_id: 0, confidential: 1}}],
                                as: "leaked",
                            },
                        },
                    ],
                },
            },
        ]);
    });
});

// A valid operator plus an injected mongotQuery override, and plus metadataMergeProtocolVersion.
describe("valid operator plus injected internal fields must not leak", function () {
    it("does not leak via an injected mongotQuery override", function () {
        assertNoLeak([
            {
                $search: {
                    index: indexName,
                    text: {query: "hello", path: "body"},
                    mongotQuery: {index: indexName, text: {query: "hello", path: "body"}},
                },
            },
        ]);
    });

    it("does not leak via an injected metadataMergeProtocolVersion", function () {
        assertNoLeak([
            {
                $search: {
                    index: indexName,
                    text: {query: "hello", path: "body"},
                    metadataMergeProtocolVersion: 1,
                },
            },
        ]);
    });
});
