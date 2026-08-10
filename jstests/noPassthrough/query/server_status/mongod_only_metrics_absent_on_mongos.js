/**
 * Verifies that serverStatus metrics that only make sense on a mongod (non-leading stage pushdown
 * counters, per-stage spilling counters, etc.) are absent from a mongos, while still being present
 * on a shard mongod.

 *
 * NB: This is a test of absence. The presence assertion on the mongods exists so that renaming or
 * removing one of these sections fails the test instead of letting it pass vacuously; update the
 * list below along with any such change.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Sections under serverStatus().metrics whose counters are only incremented on a mongod, mapped
// to metrics within them that are expected to remain visible on mongos.
const mongodOnlySections = {
    "query.nonLeadingPushdown": [],
    "query.group": [],
    "query.setWindowFields": [],
    "query.graphLookup": [],
    "query.textOr": [],
    "query.bucketAuto": [],
    "query.geoNear": [],
    "query.hashJoin": [],
    "query.lookup": [],
    "query.lookupUnwind": [],
    "query.pathArrayness": [],
    "query.spilling": [],
    // The sort totals are also reported by mongos, which can sort in memory when merging.
    "query.sort": ["totalKeysSorted", "totalBytesSorted"],
};

function getSection(metrics, dottedPath) {
    let value = metrics;
    for (const part of dottedPath.split(".")) {
        if (value === undefined || value === null) {
            return undefined;
        }
        value = value[part];
    }
    return value;
}

// Returns the dotted paths of all leaf metrics in a section, relative to the section root.
function leafMetrics(section, prefix = "") {
    let out = [];
    for (const key of Object.keys(section)) {
        const value = section[key];
        const isSubtree =
            value !== null &&
            ["[object Object]", "[object BSON]"].includes(Object.prototype.toString.call(value)) &&
            typeof value.toNumber !== "function";
        if (isSubtree) {
            out = out.concat(leafMetrics(value, prefix + key + "."));
        } else {
            out.push(prefix + key);
        }
    }
    return out;
}

function assertSectionsPresent(metrics, description) {
    for (const sectionPath of Object.keys(mongodOnlySections)) {
        const section = getSection(metrics, sectionPath);
        assert.neq(undefined, section, `section should be present on ${description}`, {
            sectionPath,
        });
        assert.gt(
            leafMetrics(section).length,
            0,
            `section should contain metrics on ${description}`,
            {sectionPath},
        );
    }
}

describe("mongod-only serverStatus metrics in a sharded cluster", function () {
    let st;
    let mongosMetrics;
    let shardMetrics;

    before(function () {
        st = new ShardingTest({shards: 1, mongos: 1});
        mongosMetrics = assert.commandWorked(st.s.adminCommand({serverStatus: 1})).metrics;
        shardMetrics = assert.commandWorked(
            st.rs0.getPrimary().adminCommand({serverStatus: 1}),
        ).metrics;
    });

    after(function () {
        st.stop();
    });

    it("exposes the metrics on a shard mongod", function () {
        assertSectionsPresent(shardMetrics, "a shard mongod");
    });

    it("does not expose the metrics on mongos", function () {
        for (const [sectionPath, allowedOnMongos] of Object.entries(mongodOnlySections)) {
            // Discover the metrics from the shard, so ones added later are covered automatically.
            const discovered = leafMetrics(getSection(shardMetrics, sectionPath));
            for (const metric of discovered) {
                const path = `${sectionPath}.${metric}`;
                if (allowedOnMongos.includes(metric)) {
                    continue;
                }
                assert.eq(
                    undefined,
                    getSection(mongosMetrics, path),
                    "mongod-only metric should be absent on mongos",
                    {path},
                );
            }
        }
    });
});

describe("mongod-only serverStatus metrics on a standalone mongod", function () {
    let conn;
    let metrics;

    before(function () {
        conn = MongoRunner.runMongod({});
        metrics = assert.commandWorked(conn.adminCommand({serverStatus: 1})).metrics;
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    // Every mongod has a ShardServer-role service, even when it is not part of a sharded
    // cluster, so scoping these metrics to ClusterRole::ShardServer must not hide them here.
    it("exposes the metrics on a non-sharded mongod", function () {
        assertSectionsPresent(metrics, "a standalone mongod");
    });
});
