/**
 * Verify multikey catalog state is identical across replica set members.
 *
 * For each replica set (or each shard's replica set + the config server in a sharded
 * cluster), the hook:
 *   1. Reads primary's lastApplied as T. Secondaries' snapshot read at T waits implicitly
 *      via afterClusterTime semantics.
 *   2. For each collection (sampled if `maxCollectionsPerIteration` set):
 *      - Regular index: `$listCatalog` snapshot-read on each member at T; compares
 *        `md.indexes[].multikey` boolean + `md.indexes[].multikeyPaths` BinData
 *        byte-equal across members.
 *      - Wildcard index: samples one doc on primary, derives field paths, runs `explain`
 *        of `find({<path>: 1}).hint(<wcIdx>)` at snapshot T on each member, extracts
 *        `IXSCAN.multiKeyPaths[<path>]` (the planner's view of multikey paths for this
 *        wildcard field at T), and asserts the set is identical across members.
 *
 * Initial-sync filter: members in STARTUP / STARTUP2 / RECOVERING are excluded from the
 * compare set.
 *
 * Configuration via TestData.multikeyHook:
 *   - maxCollectionsPerIteration (int | null): cap on collections checked per iteration.
 *     Default null = unlimited.
 *   - maxWildcardPathsPerIndex (int): cap on field paths sampled per wildcard index per
 *     iteration. Default 10.
 *
 * Failure: any divergence throws an assertion.
 */
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";
import {Thread} from "jstests/libs/parallelTester.js";

if (typeof db === "undefined") {
    throw new Error(
        "Expected mongo shell to be connected a server, but global 'db' object isn't defined",
    );
}

TestData = TestData || {};
TestData.traceExceptions = false;
TestData.disableImplicitSessions = true;

async function checkMultikeyConsistencyForReplicaSet(hosts) {
    // This runs both directly (replica-set topology) and as a parallelTester Thread worker (one
    // per shard in a sharded cluster). A Thread worker evals this function's source in a fresh
    // scope with no access to the module's imports, top-level consts, or sibling functions, so
    // everything the check needs is resolved inside: config from TestData, shared-lib primitives
    // via dynamic import, and the comparison helpers as nested declarations.
    const {ReplSetTest} = await import("jstests/libs/replsettest.js");
    const {findIndexByName, readCatalogIndexesAtClusterTime, readWildcardMultikeyPaths} =
        await import("jstests/libs/multikey_consistency_check.js");

    const hookConfig = TestData.multikeyHook || {};
    const MAX_COLLECTIONS = hookConfig.maxCollectionsPerIteration ?? null;
    const MAX_WILDCARD_PATHS = hookConfig.maxWildcardPathsPerIndex ?? 10;

    // Array.shuffle draws from the global Random, which a freshly-eval'd worker shell has not
    // seeded; seed it before any shuffle.
    Random.setRandomSeed();

    function isWildcardIndex(idx) {
        // A wildcard index either has "$**" as a top-level key or a "$**" suffix on a key
        // (compound wildcard).
        for (const k of Object.keys(idx.key)) {
            if (k === "$**" || k.endsWith(".$**")) return true;
        }
        return false;
    }

    function readRegularMultikeyState(conn, dbName, collName, indexName, T) {
        const indexes = readCatalogIndexesAtClusterTime(conn, dbName, collName, T);
        if (indexes === undefined || indexes === null) return indexes;
        const idxMd = findIndexByName(indexes, indexName);
        if (!idxMd) return null;
        return {
            multikey: idxMd.multikey,
            multikeyPaths: idxMd.multikeyPaths,
        };
    }

    function assertRegularMultikeyConsistent(members, dbName, collName, indexName, T) {
        const ns = `${dbName}.${collName}`;
        const states = members.map((c) =>
            readRegularMultikeyState(c, dbName, collName, indexName, T),
        );
        let ref = null;
        let refHost = null;
        for (let i = 0; i < states.length; i++) {
            const s = states[i];
            // undefined: this member's snapshot read soft-failed (WriteConflict/SnapshotUnavailable);
            // null: collection/index absent at T on this member. Either way there is nothing to
            // compare against, so skip it rather than treating it as a divergence.
            if (s === undefined || s === null) continue;
            if (ref === null) {
                ref = s;
                refHost = members[i].host;
                continue;
            }
            // Path-level multikey tracking only exists for B-tree and 2dsphere. text/hashed/2d
            // indexes (and pre-3.4 legacy indexes) store only the multikey boolean in the
            // catalog, leaving `multikeyPaths` undefined. Compare the boolean unconditionally;
            // delegate path comparison to bsonBinaryEqual only when both sides have BinData.
            // A one-sided BinData is itself a divergence.
            const refHasPaths = ref.multikeyPaths !== undefined;
            const sHasPaths = s.multikeyPaths !== undefined;
            let same = ref.multikey === s.multikey;
            if (same && (refHasPaths || sHasPaths)) {
                same =
                    refHasPaths && sHasPaths && bsonBinaryEqual(ref.multikeyPaths, s.multikeyPaths);
            }
            assert(same, "multikey divergence (regular index)", {
                ns,
                indexName,
                atClusterTime: T,
                host_ref: refHost,
                state_ref: ref,
                host_other: members[i].host,
                state_other: s,
            });
        }
    }

    function derivePathsFromDoc(doc, prefix, out) {
        for (const k of Object.keys(doc)) {
            if (k === "_id") continue;
            const path = prefix ? `${prefix}.${k}` : k;
            out.push(path);
            const v = doc[k];
            if (
                v !== null &&
                typeof v === "object" &&
                !Array.isArray(v) &&
                !(v instanceof ObjectId)
            ) {
                derivePathsFromDoc(v, path, out);
            }
        }
    }

    function assertWildcardMultikeyConsistent(members, dbName, collName, idx, T) {
        const indexName = idx.name;

        // Sample one doc to derive candidate field paths to query.
        let sampleDoc;
        try {
            sampleDoc = members[0].getDB(dbName)[collName].findOne({}, {_id: 0});
        } catch (e) {
            if (e.code === ErrorCodes.NamespaceNotFound) return;
            throw e;
        }
        if (!sampleDoc) return;

        const candidatePaths = [];
        derivePathsFromDoc(sampleDoc, "", candidatePaths);
        if (candidatePaths.length === 0) return;

        Array.shuffle(candidatePaths);
        const sampled = candidatePaths.slice(0, MAX_WILDCARD_PATHS);

        for (const fieldName of sampled) {
            const perMember = members.map((c) =>
                readWildcardMultikeyPaths(c, dbName, collName, T, indexName, fieldName),
            );

            let ref = null;
            let refHost = null;
            for (let i = 0; i < perMember.length; i++) {
                const s = perMember[i];
                // undefined: this member's explain soft-failed (e.g. WriteConflict/SnapshotUnavailable).
                // Nothing to compare against, so skip rather than flag a divergence. (A planner that
                // declines the wildcard index returns an empty set, not undefined, and stays comparable.)
                if (s === undefined) continue;
                if (ref === null) {
                    ref = s;
                    refHost = members[i].host;
                    continue;
                }
                assert.sameMembers(ref, s, "multikey divergence (wildcard index)", undefined, {
                    ns: `${dbName}.${collName}`,
                    indexName,
                    fieldName,
                    atClusterTime: T,
                    host_ref: refHost,
                    paths_ref: ref,
                    host_other: members[i].host,
                    paths_other: s,
                });
            }
        }
    }

    // Suppress the noisy ReplSetTest constructor output.
    const quietly = (func) => {
        const printOriginal = print;
        try {
            print = Function.prototype;
            func();
        } finally {
            print = printOriginal;
        }
    };

    let rst;
    quietly(() => {
        rst = new ReplSetTest(hosts[0]);
    });

    const primary = rst.getPrimary();
    if (!primary.adminCommand({serverStatus: 1}).storageEngine.supportsSnapshotReadConcern) {
        print(
            "Skipping multikey consistency check: storage engine does not support snapshot reads " +
                "on " +
                rst.getURL(),
        );
        return {ok: 1};
    }

    // Filter to currently-readable members. Initial sync and recovering members are expected to
    // diverge; arbiters have no data.
    const statusByHost = {};
    const members = [];
    for (const conn of [primary, ...rst.getSecondaries()]) {
        const status = assert.commandWorked(conn.adminCommand({replSetGetStatus: 1}));
        const self = status.members.find((m) => m.self);
        statusByHost[conn.host] = self;
        if (self.stateStr !== "PRIMARY" && self.stateStr !== "SECONDARY") {
            continue;
        }
        if (conn.adminCommand({isMaster: 1}).arbiterOnly) {
            continue;
        }
        members.push(conn);
    }
    if (members.length < 2) {
        print(
            "Skipping multikey consistency check: fewer than 2 readable members on " + rst.getURL(),
        );
        return {ok: 1};
    }

    // Use primary's lastApplied as the snapshot timestamp. Secondaries receiving the snapshot
    // read at this T will block until they catch up.
    const T = statusByHost[primary.host].optime.ts;
    if (!T) {
        return {ok: 1};
    }

    // Discover the namespaces and indexes to check. Use the primary as the source of truth
    // for what collections exist; per-namespace compare happens at T on every member.
    const dbs = assert.commandWorked(primary.adminCommand({listDatabases: 1})).databases;
    const userDbs = dbs.filter((d) => !["admin", "config", "local"].includes(d.name));

    const allColls = [];
    for (const d of userDbs) {
        let collInfos;
        try {
            collInfos = primary.getDB(d.name).getCollectionInfos({type: "collection"});
        } catch (e) {
            // A test may transiently leave an invalid view in system.views. Listing collections
            // throws InvalidViewDefinition, so we just skip this database.
            if (e.code === ErrorCodes.InvalidViewDefinition) {
                continue;
            }
            throw e;
        }
        for (const c of collInfos) {
            if (c.name.startsWith("system.")) continue;
            allColls.push({dbName: d.name, collName: c.name});
        }
    }

    if (MAX_COLLECTIONS !== null && allColls.length > MAX_COLLECTIONS) {
        Array.shuffle(allColls);
        allColls.length = MAX_COLLECTIONS;
    }

    for (const {dbName, collName} of allColls) {
        let indexes;
        try {
            indexes = primary.getDB(dbName)[collName].getIndexes();
        } catch (e) {
            // Race: between enumeration and this read a concurrent workload may have dropped the
            // collection (NamespaceNotFound) or replaced it with a view of the same name
            // (CommandNotSupportedOnView). Both mean there is no collection to check now; skip.
            if (
                e.code === ErrorCodes.NamespaceNotFound ||
                e.code === ErrorCodes.CommandNotSupportedOnView
            ) {
                continue;
            }
            throw e;
        }

        for (const idx of indexes) {
            if (isWildcardIndex(idx)) {
                assertWildcardMultikeyConsistent(members, dbName, collName, idx, T);
            } else if (idx.name !== "_id_") {
                // Skip the _id index: it can never be multikey.
                assertRegularMultikeyConsistent(members, dbName, collName, idx.name, T);
            }
        }
    }

    return {ok: 1};
}

// ---- Topology entry point ---------------------------------------------------------

const topology = DiscoverTopology.findConnectedNodes(db.getMongo());

if (topology.type === Topology.kReplicaSet) {
    const res = await checkMultikeyConsistencyForReplicaSet(topology.nodes);
    assert.commandWorked(res, () => "multikey consistency check failed: " + tojson(res));
} else if (topology.type === Topology.kShardedCluster) {
    const threads = [];
    try {
        if (topology.configsvr && topology.configsvr.nodes && topology.configsvr.nodes.length > 1) {
            const thread = new Thread(
                checkMultikeyConsistencyForReplicaSet,
                topology.configsvr.nodes,
            );
            threads.push(thread);
            thread.start();
        }
        for (const shardName of Object.keys(topology.shards)) {
            const shard = topology.shards[shardName];
            if (shard.type !== Topology.kReplicaSet) continue;
            if (shard.nodes.length < 2) continue;
            const thread = new Thread(checkMultikeyConsistencyForReplicaSet, shard.nodes);
            threads.push(thread);
            thread.start();
        }
    } finally {
        let exception;
        const returnData = threads.map((thread) => {
            try {
                thread.join();
                return thread.returnData();
            } catch (e) {
                if (!exception) exception = e;
            }
        });
        if (exception) {
            // eslint-disable-next-line
            throw exception;
        }
        returnData.forEach((res) => {
            assert.commandWorked(
                res,
                () => "multikey consistency check (point-in-time) failed: " + tojson(res),
            );
        });
    }
} else {
    throw new Error("Unsupported topology configuration: " + tojson(topology));
}
