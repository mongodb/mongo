/**
 * Tests that an external (non-__system) client can run hybrid-search aggregations
 * ($rankFusion / $scoreFusion) whose effective pipeline contains $search reached through view
 * resolution or hybrid-search desugar. These paths serialize-and-reparse a pipeline that includes
 * a search stage, and rely on SerializationOptions::serializeForReparse to emit the stage in
 * user-facing form. If that flag stops propagating on any path, the re-parsed pipeline carries
 * internal routing fields (e.g. mongotQuery) and the recursive LiteParse check rejects the query
 * for external clients with 5491300 — even though the user never typed those fields.
 *
 * Default mongot_e2e_* connections authenticate as __system, which bypasses the external-client
 * check; this test creates a non-privileged user and runs every case through that connection.
 *
 * @tags: [featureFlagSearchHybridScoringFull, requires_fcv_82]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";

// We use the admin db for the non-privileged user that simulates an external client, and a
// per-run sibling db (UUID-suffixed) for the test collection, the views built over it, and the
// hybrid-search queries themselves. The sibling db gives us db-level isolation so that burn-in
// (which runs the same file across multiple parallel workers against one fixture) doesn't race
// on shared namespaces; only the user needs to be UUID-suffixed since users live on admin.
// Use a short hand-chosen prefix (not jsTestName) so the resulting db name stays under
// MongoDB's 63-char limit.
const adminDb = db.getSiblingDB("admin");

const kSuffix = extractUUIDFromObject(UUID());
const kTestDbName = "hybrid_search_db_" + kSuffix;
const testDb = db.getSiblingDB(kTestDbName);

const kCollName = "coll";
const kSearchViewName = "searchView"; // direct view: [{$search: ...}]
const kNestedViewName = "nestedSearchView"; // view-on-view: [{$match: ...}] on searchView
const kSearchIndexName = "searchIdx";

const kTestUser = "externalHybridSearchUser_" + kSuffix;
const kTestPwd = "externalHybridSearchPwd";

let extDb;

before(function () {
    assert.commandWorked(
        testDb[kCollName].insertMany([
            {_id: 1, body: "hello world", score: 5},
            {_id: 2, body: "hello again", score: 7},
            {_id: 3, body: "goodbye world", score: 3},
        ]),
    );

    createSearchIndex(testDb[kCollName], {
        name: kSearchIndexName,
        definition: {mappings: {dynamic: true}},
    });

    // Note: do not pre-drop view namespaces via testDb[viewName].drop() — the
    // implicitly_shard_accessed_collections.js override (enabled in mongot_e2e_sharded_collections)
    // hooks DB.prototype.getCollection and would shard-create the view name as a collection
    // first, after which createView fails with NamespaceExists.
    assert.commandWorked(
        testDb.createView(kSearchViewName, kCollName, [
            {$search: {index: kSearchIndexName, text: {query: "hello", path: "body"}}},
        ]),
    );
    assert.commandWorked(
        testDb.createView(kNestedViewName, kSearchViewName, [{$match: {score: {$gt: 0}}}]),
    );

    adminDb.dropUser(kTestUser);
    assert.commandWorked(
        adminDb.runCommand({
            createUser: kTestUser,
            pwd: kTestPwd,
            roles: [{role: "read", db: kTestDbName}],
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
    extDb = extConn.getDB(kTestDbName);
});

after(function () {
    try {
        dropSearchIndex(testDb[kCollName], {name: kSearchIndexName});
    } catch (e) {
        jsTest.log.info("dropSearchIndex during cleanup raised; ignoring", {error: e});
    }
    // Drop the whole per-run db rather than each namespace individually — simpler, idempotent
    // even if before() failed partway and left some namespaces uncreated.
    assert.commandWorked(testDb.dropDatabase());
    adminDb.dropUser(kTestUser);
});

function runAsExternal(targetCollOrView, pipeline) {
    return extDb.runCommand({aggregate: targetCollOrView, cursor: {}, pipeline});
}

describe("hybrid search as an external client", function () {
    // Case 1: canonical view-resolution path through pipeline_resolver.cpp. If
    // serializeForReparse is dropped, the resolved pipeline carries mongotQuery and
    // lite-parse rejects with 5491300.
    it("$rankFusion on a search-indexed view", function () {
        const pipeline = [
            {
                $rankFusion: {
                    input: {
                        pipelines: {
                            a: [{$sort: {score: -1}}],
                        },
                    },
                },
            },
        ];
        assert.commandWorked(runAsExternal(kSearchViewName, pipeline));
    });

    // Case 2: same path as case 1, different hybrid operator — guards against accidental
    // $rankFusion-only coupling.
    it("$scoreFusion on a search-indexed view", function () {
        const pipeline = [
            {
                $scoreFusion: {
                    input: {
                        pipelines: {
                            a: [{$score: {score: "$score", normalization: "none"}}],
                        },
                        normalization: "sigmoid",
                    },
                    combination: {method: "avg"},
                },
            },
        ];
        assert.commandWorked(runAsExternal(kSearchViewName, pipeline));
    });

    // Case 3: multiple $search input pipelines to $rankFusion. The desugarer wraps every
    // non-first input pipeline in a $unionWith and serializes it via Pipeline::serializeToBson
    // (hybrid_search_pipeline_builder.cpp). Without serializeForReparse on that serialize call,
    // the inner $search emits internal-form BSON with mongotQuery, and the reparse rejects with
    // 5491300 under the external user.
    it("$rankFusion with multiple $search input pipelines (no view)", function () {
        const pipeline = [
            {
                $rankFusion: {
                    input: {
                        pipelines: {
                            a: [
                                {
                                    $search: {
                                        index: kSearchIndexName,
                                        text: {query: "hello", path: "body"},
                                    },
                                },
                            ],
                            b: [
                                {
                                    $search: {
                                        index: kSearchIndexName,
                                        text: {query: "world", path: "body"},
                                    },
                                },
                            ],
                        },
                    },
                },
            },
        ];
        assert.commandWorked(runAsExternal(kCollName, pipeline));
    });

    // Case 4: view-on-view. Multiple layers of view resolution must all honor
    // serializeForReparse for the final lite-parse to see user-form $search.
    it("$rankFusion on a nested view whose underlying view contains $search", function () {
        const pipeline = [
            {
                $rankFusion: {
                    input: {
                        pipelines: {
                            a: [{$sort: {score: -1}}],
                        },
                    },
                },
            },
        ];
        assert.commandWorked(runAsExternal(kNestedViewName, pipeline));
    });
});
