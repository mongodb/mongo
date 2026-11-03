/**
 * Verifies mongod and mongos under Landlock filesystem self-sandboxing (--landlockMode /
 * security.landlock.mode): servers start, serve basic CRUD traffic, and rotate their log
 * files with the sandbox enforced, and report the sandbox state through the "landlock"
 * serverStatus section -- the stable surface for monitoring and tests: the mode in effect,
 * the Landlock ABI version probed from the running kernel (reported even while the sandbox
 * is disabled; 0 only when the kernel lacks Landlock or has it disabled), whether the
 * sandbox is actually enforced ("active"), and -- only while active -- the access rights the
 * ruleset handles and the requested rights the running kernel could not restrict, grouped by
 * rule type ("fs").
 *
 * Landlock is Linux-only, and the mode decides what happens on a kernel that cannot enforce
 * it: "bestEffort" starts unsandboxed, "enforce" refuses to start, and "disabled" skips
 * Landlock entirely. Nodes here run "bestEffort" (the shipped default is "disabled"), and a
 * standalone probe decides whether this kernel enforces the sandbox at all; full enforcement
 * is asserted only when it does. A half-applied policy is never tolerated: once Landlock is
 * available, any failure to apply the policy is fatal in the server itself, whatever the mode,
 * so such a node fails to start rather than reaching an assertion here.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    assertLandlockMode,
    logEntriesWithId,
    probeLandlockSupport,
} from "jstests/noPassthrough/security/landlock_helpers.js";

if (getBuildInfo().buildEnvironment.target_os !== "linux") {
    jsTest.log.info("Landlock is Linux-only; skipping test");
    quit();
}

// Every sandboxed node starts with its log going to a file: the Landlock startup lines are
// among the first a node emits, so on a busy node they rotate out of the in-memory getLog
// buffer, and assertions read the on-disk log instead.
const kNodeOpts = {landlockMode: "bestEffort", useLogFiles: true};

const kIdRuleApplied = 13118805; // one path rule granted (attr lists the rights)
const kIdRulesetApplied = 13118812; // sandbox fully enforced
const kIdDisabled = 13118814; // mode is "disabled"; Landlock never initialized
const kIdBestEffortDowngrade = 13253501; // this host cannot enforce Landlock; carrying on
// Every way the sandbox can fail to apply once Landlock is available -- 13118803 and 13118806
// (ruleset creation), 13118809 (a rejected path rule), 13118811 (the enforcement syscall) -- is
// fatal in the server, so a node that hit one never lives to be inspected here. Nor does
// 13253500 (enforce refusing to start), which is always followed by a uassert. Those outcomes
// are caught by the node failing to start, not by an assertion on its log.

function logContains(node, id) {
    return cat(node.fullOptions.logFile).includes(`"id":${id}`);
}

/**
 * Asserts that `node` reached a clean sandboxing decision -- either full enforcement or an
 * explicit best-effort downgrade on an unsupporting kernel -- and returns whether the sandbox
 * engaged. A half-applied policy needs no assertion: the server treats it as fatal, so such a
 * node never starts and is caught before this runs.
 */
function assertSandboxOutcome(node) {
    const attr = {host: node.host, logFile: node.fullOptions.logFile};
    assert(!logContains(node, kIdDisabled), "Landlock was disabled on node", attr);

    const applied = logContains(node, kIdRulesetApplied);
    assert(
        applied || logContains(node, kIdBestEffortDowngrade),
        "node logged neither Landlock enforcement nor a best-effort downgrade",
        attr,
    );
    return applied;
}

function assertBasicCrud(coll) {
    const kNumDocs = 100;
    const docs = [];
    for (let i = 0; i < kNumDocs; i++) {
        docs.push({_id: i, x: i});
    }
    assert.commandWorked(coll.insert(docs));
    assert.eq(kNumDocs, coll.countDocuments({}));

    assert.eq(42, coll.findOne({_id: 42}).x);
    assert.eq(kNumDocs / 2, coll.find({x: {$gte: kNumDocs / 2}}).itcount());

    assert.commandWorked(coll.remove({x: {$lt: 10}}));
    assert.eq(kNumDocs - 10, coll.countDocuments({}));
    assert.commandWorked(coll.remove({}));
    assert.eq(0, coll.countDocuments({}));
}

// Whether the running kernel enforces Landlock, decided by the standalone probe below; the
// sharded cluster must then reach the same decision on every node.
const kernel = probeLandlockSupport();
jsTest.log.info("Landlock sandbox status on this kernel", {kernel});

describe("standalone mongod under the Landlock sandbox", function () {
    let conn;

    before(function () {
        conn = MongoRunner.runMongod(kNodeOpts);
        assert.neq(null, conn, "mongod failed to start with --landlockMode=bestEffort");
        assertLandlockMode(conn, "bestEffort");
    });

    it("reaches a clean sandboxing decision", function () {
        assertSandboxOutcome(conn);
    });

    // The server never exec()s files, so execution must be denied everywhere:
    // the ruleset handles EXECUTE (deny by default) and no path rule may
    // re-grant it.
    it("handles but never grants EXECUTE", function () {
        if (!kernel.active) {
            jsTest.log.info("kernel does not enforce Landlock; nothing to assert");
            return;
        }
        const kExecute = "LANDLOCK_ACCESS_FS_EXECUTE";

        const [ruleset] = logEntriesWithId(conn, kIdRulesetApplied);
        assert(ruleset.attr.handledAccessRights.fs.includes(kExecute), "EXECUTE is not handled", {
            ruleset,
        });

        const rules = logEntriesWithId(conn, kIdRuleApplied);
        assert.gt(rules.length, 0, "no Landlock path rules were logged");
        for (const rule of rules) {
            assert(!rule.attr.allowedAccess.includes(kExecute), "a rule granted EXECUTE", {rule});
        }
    });

    it("reports the sandbox state in serverStatus", function () {
        const landlock = assertLandlockMode(conn, "bestEffort");
        // The section must agree with the enforcement outcome derived from the startup log.
        assert.eq(kernel.active, landlock.active, "serverStatus and the startup log disagree", {
            landlock,
        });
    });

    it("serves basic CRUD", function () {
        assertBasicCrud(conn.getDB(jsTestName()).coll);
    });

    // Log rotation renames the live log file and recreates it, the exact rename-and-recreate
    // pattern the sandbox policy must permit in the log directory. Runs last: the assertions
    // above read the pre-rotation file.
    it("rotates its log file", function () {
        assert.commandWorked(conn.adminCommand({logRotate: 1}));
        assert.commandWorked(conn.adminCommand({ping: 1}));
    });

    after(function () {
        if (conn) {
            MongoRunner.stopMongod(conn);
        }
    });
});

describe("security.landlock.mode", function () {
    it("defaults to disabled, still reporting the probed ABI version", function () {
        const conn = MongoRunner.runMongod({});
        assert.neq(null, conn, "mongod failed to start with no landlock option");
        try {
            assertLandlockMode(conn, "disabled");
        } finally {
            MongoRunner.stopMongod(conn);
        }
    });

    // "disabled" skips initialization outright, so no ruleset and no path rule may be applied.
    it("skips Landlock initialization entirely when disabled", function () {
        const conn = MongoRunner.runMongod({landlockMode: "disabled", useLogFiles: true});
        try {
            const attr = {logFile: conn.fullOptions.logFile};
            const landlock = assertLandlockMode(conn, "disabled");
            assert(logContains(conn, kIdDisabled), "disabled mode was not logged", attr);
            assert(!logContains(conn, kIdRulesetApplied), "a ruleset was applied", attr);
            assert(!logContains(conn, kIdRuleApplied), "a path rule was applied", attr);
        } finally {
            MongoRunner.stopMongod(conn);
        }
    });

    // "enforce" differs from "bestEffort" only when the kernel cannot enforce the sandbox: it
    // must refuse to start rather than run unrestricted, so which branch applies here depends
    // on what the standalone probe above found this kernel can do.
    it("under enforce, either enforces the sandbox or refuses to start", function () {
        // Both outcomes are legitimate and the kernel decides which, so the launch is attempted
        // either way and whichever happened is then checked for consistency. Refusing to start
        // shows up as a throw, since mongod exits non-zero before accepting connections.
        let conn = null;
        try {
            conn = MongoRunner.runMongod({landlockMode: "enforce", useLogFiles: true});
        } catch (e) {
            jsTest.log.info("mongod refused to start under enforce", {error: e.toString()});
        }
        if (!conn) {
            assert.neq(
                true,
                kernel.active,
                "enforce refused to start on a kernel that does enforce Landlock",
            );
            return;
        }
        try {
            const landlock = assertLandlockMode(conn, "enforce");
            // Starting without enforcing is the one outcome enforce exists to rule out.
            assert.eq(true, landlock.active, "enforce started without enforcing", {landlock});
        } finally {
            MongoRunner.stopMongod(conn);
        }
    });

    // An unrecognized mode is an operator error, and must stop startup rather than leave the
    // sandbox at some silently chosen strength.
    it("refuses to start on an unrecognized mode", function () {
        assert.throws(
            () => MongoRunner.runMongod({landlockMode: "bogus"}),
            [],
            "mongod started with an unrecognized --landlockMode",
        );
    });
});

describe("sharded cluster under the Landlock sandbox", function () {
    let st;

    before(function () {
        st = new ShardingTest({
            shards: 2,
            rs: {nodes: 1},
            mongos: 1,
            other: {
                rsOptions: kNodeOpts,
                configOptions: kNodeOpts,
                mongosOptions: kNodeOpts,
            },
        });
    });

    it("sandboxes every mongod and the mongos", function () {
        for (const node of [st.rs0.getPrimary(), st.rs1.getPrimary(), st.configRS.getPrimary()]) {
            assert.eq(kernel.active, assertSandboxOutcome(node), "mongod outcome diverged", {
                host: node.host,
            });
        }
        assert.eq(kernel.active, assertSandboxOutcome(st.s), "mongos outcome diverged", {
            host: st.s.host,
        });
    });

    it("reports the sandbox state in serverStatus on every node", function () {
        for (const [node, desc] of [
            [st.s, "mongos"],
            [st.rs0.getPrimary(), "shard0 primary"],
            [st.rs1.getPrimary(), "shard1 primary"],
            [st.configRS.getPrimary(), "config server primary"],
        ]) {
            const landlock = assertLandlockMode(node, "bestEffort");
            assert.eq(
                kernel.active,
                landlock.active,
                "serverStatus and the standalone probe disagree",
                {desc, landlock},
            );
        }
    });

    it("serves CRUD through mongos against both sandboxed shards", function () {
        const dbName = jsTestName();
        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
        );
        assert.commandWorked(
            st.s.adminCommand({shardCollection: `${dbName}.coll`, key: {_id: "hashed"}}),
        );
        assertBasicCrud(st.s.getDB(dbName).coll);
    });

    after(function () {
        if (st) {
            st.stop();
        }
    });
});
