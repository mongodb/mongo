/**
 * Verifies that mongos's retry backoff honors the `baseBackoffMS` a shard sends back on rejection.
 *
 * The test drives a genuine ingress request rate limiter (IRRL) rejection on the mongos -> shard
 * fan-out and asserts that the resulting delta in per-shard `totalBackoffTimeMillis` reflects the
 * `baseBackoffMS` the shard sent back.
 *
 * @tags: [requires_fcv_83]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    disableRateLimiter,
    enableZeroBurstRateLimiter,
    getShardStats,
    kInternalConnectionAppNameExemptions,
    kKeyFile,
    kRateLimiterExemptAppName,
    setParameter,
    setupAuth,
    shardingStatisticsDifference,
} from "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js";

// Drop "MongoDB Internal Client" -- it prefix-matches the driver name of mongos's forwarded
// connections, which would exempt the mongos -> shard fan-out from the ingress request rate limiter.
const kSafeInternalExemptions = [
    kRateLimiterExemptAppName,
    ...kInternalConnectionAppNameExemptions.filter((name) => name !== "MongoDB Internal Client"),
];

// The shard sends this baseBackoffMS back on a rejection; the mongos-side backoff should be at
// least this large. Jitter is disabled below so the observed backoff is deterministic.
const kBaseBackoffMS = 200;

describe("mongos honors a baseBackoffMS from a IRRL rejection", function () {
    let st;
    let exemptConn;
    let testDB;
    let shardPrimary;
    let shardName;

    before(function () {
        st = new ShardingTest({
            mongos: 1,
            shards: 1,
            other: {
                auth: "",
                keyFile: kKeyFile,
                mongosOptions: {setParameter: {ingressRequestRateLimiterEnabled: false}},
                rsOptions: {setParameter: {ingressRequestRateLimiterEnabled: false}},
            },
        });

        exemptConn = new Mongo(`mongodb://${st.s.host}/?appName=${kRateLimiterExemptAppName}`);
        setupAuth(st.s, exemptConn);
        testDB = exemptConn.getDB("test");

        assert.commandWorked(exemptConn.adminCommand({enableSharding: "test"}));
        assert.commandWorked(
            exemptConn.adminCommand({shardCollection: "test.coll", key: {_id: 1}}),
        );
        // Several documents so a batchSize: 1 cursor stays open on the shard for the getMore case.
        assert.commandWorked(
            testDB.coll.insert([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}]),
        );

        shardPrimary = st.rs0.getPrimary();
        shardName = st.shard0.shardName;
    });

    after(function () {
        st.stop();
    });

    function setShardBaseBackoffMS(baseBackoffMS) {
        authutil.asCluster(shardPrimary, kKeyFile, () => {
            assert.commandWorked(
                shardPrimary.adminCommand({
                    setParameter: 1,
                    externalClientBaseBackoffMS: baseBackoffMS,
                }),
            );
        });
    }

    // Drives `testCase` against a shard that rejects with `kBaseBackoffMS` and asserts that the
    // backoff time mongos spent retrying the shard reflects that baseBackoffMS. All rate-limiter
    // and client-retry state is restored before returning.
    function assertHonorsBaseBackoffMS({name, setup, run}) {
        setShardBaseBackoffMS(kBaseBackoffMS);

        // Any per-case setup (e.g. opening a cursor for getMore) must run before the rate limiter is turned on.
        const state = setup ? setup() : undefined;

        // Force every request to rejected by the rate limiter and make the mongos-side backoff
        // deterministic (no jitter, known retry knobs).
        enableZeroBurstRateLimiter(shardPrimary, kSafeInternalExemptions);
        assert.commandWorked(
            exemptConn.adminCommand({
                configureFailPoint: "returnMaxBackoffDelay",
                mode: "alwaysOn",
            }),
        );
        const savedParams = setParameter(exemptConn, {
            defaultClientMaxRetryAttempts: 2,
            defaultClientBaseBackoffMillis: 1,
            defaultClientMaxBackoffMillis: 100000,
        });

        try {
            // Run the command and measure the backoff time mongos spent retrying this shard; it should be at least the
            // baseBackoffMS the shard sent back.
            const statsBefore = getShardStats(exemptConn, shardName);
            run(state);
            const statsAfter = getShardStats(exemptConn, shardName);
            const totalBackoffTimeMillis = shardingStatisticsDifference(
                statsAfter,
                statsBefore,
            ).totalBackoffTimeMillis;
            assert.gte(
                totalBackoffTimeMillis,
                kBaseBackoffMS,
                "expected backoff to reflect the baseBackoffMS the shard sent back",
                {command: name, totalBackoffTimeMillis},
            );
        } finally {
            // Undo everything the setup above changed.
            assert.commandWorked(exemptConn.adminCommand({setParameter: 1, ...savedParams}));
            assert.commandWorked(
                exemptConn.adminCommand({configureFailPoint: "returnMaxBackoffDelay", mode: "off"}),
            );
            disableRateLimiter(shardPrimary.host);
            setShardBaseBackoffMS(0);
        }
    }

    const assertRejectedByRateLimiter = (cmd) =>
        assert.commandFailedWithCode(
            testDB.runCommand(cmd),
            ErrorCodes.IngressRequestRateLimitExceeded,
        );

    const testCases = [
        {
            name: "find",
            run: () => assertRejectedByRateLimiter({find: "coll", filter: {}}),
        },
        {
            name: "aggregate",
            run: () => assertRejectedByRateLimiter({aggregate: "coll", pipeline: [], cursor: {}}),
        },
        {
            name: "insert",
            run: () => assertRejectedByRateLimiter({insert: "coll", documents: [{a: 1}]}),
        },
        {
            name: "getMore",
            setup: () => {
                const res = assert.commandWorked(
                    testDB.runCommand({find: "coll", filter: {}, batchSize: 1}),
                );
                assert.neq(res.cursor.id, 0, "expected an open cursor for the getMore case", {res});
                return {cursorId: res.cursor.id};
            },
            run: (state) =>
                assertRejectedByRateLimiter({
                    getMore: state.cursorId,
                    collection: "coll",
                    batchSize: 1,
                }),
        },
    ];

    testCases.forEach((tc) => {
        it(`honors the baseBackoffMS for ${tc.name}`, function () {
            assertHonorsBaseBackoffMS(tc);
        });
    });
});
