/**
 * Verifies mongod and mongos under Landlock filesystem self-sandboxing (--landlock /
 * security.landlock.enabled): servers start, serve basic CRUD traffic, and rotate their log
 * files with the sandbox enforced, and report the sandbox state through the "landlock"
 * serverStatus section -- the stable surface for monitoring and tests: whether the option is
 * enabled, the Landlock ABI version probed from the running kernel (reported even while the
 * option is off; 0 only when the kernel lacks Landlock or has it disabled), whether the
 * sandbox is actually enforced ("active"), and -- only while active -- the access rights the
 * ruleset handles and the requested rights the running kernel could not restrict, grouped by
 * rule type ("fs").
 *
 * Landlock is Linux-only and applied best-effort: on a kernel without Landlock support the
 * server deliberately starts unsandboxed. A standalone probe decides whether this kernel
 * enforces the sandbox, and full enforcement is asserted only when it does; a half-applied
 * policy (a rejected path rule, a failed enforcement syscall) is always a failure.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

if (getBuildInfo().buildEnvironment.target_os !== "linux") {
    jsTest.log.info("Landlock is Linux-only; skipping test");
    quit();
}

// Every sandboxed node starts with its log going to a file: the Landlock startup lines are
// among the first a node emits, so on a busy node they rotate out of the in-memory getLog
// buffer, and assertions read the on-disk log instead.
const kNodeOpts = {landlock: "true", useLogFiles: true};

const kIdRuleApplied = 13118805; // one path rule granted (attr lists the rights)
const kIdRulesetApplied = 13118812; // sandbox fully enforced
const kIdRulesetCreateFailed = 13118806; // kernel lacks/disables Landlock; best-effort continue
const kIdAddRuleFailed = 13118809; // policy bug: the kernel rejected a path rule
const kIdRestrictSelfFailed = 13118811; // enforcement syscall failed
const kIdSkippedDisabled = 13118814; // the enable option never reached the server

function logContains(node, id) {
    return cat(node.fullOptions.logFile).includes(`"id":${id}`);
}

function logEntries(node, id) {
    return cat(node.fullOptions.logFile)
        .split("\n")
        .filter((line) => line.includes(`"id":${id}`))
        .map((line) => JSON.parse(line));
}

/**
 * Asserts that `node` reached a clean sandboxing decision -- either full enforcement or an
 * explicit best-effort downgrade on an unsupporting kernel, never a half-applied policy --
 * and returns whether the sandbox engaged.
 */
function assertSandboxOutcome(node) {
    const attr = {host: node.host, logFile: node.fullOptions.logFile};
    assert(!logContains(node, kIdSkippedDisabled), "Landlock was never enabled on node", attr);
    assert(!logContains(node, kIdAddRuleFailed), "the kernel rejected a Landlock path rule", attr);
    assert(!logContains(node, kIdRestrictSelfFailed), "Landlock enforcement failed", attr);

    const applied = logContains(node, kIdRulesetApplied);
    assert(
        applied || logContains(node, kIdRulesetCreateFailed),
        "node logged neither Landlock enforcement nor a best-effort downgrade",
        attr,
    );
    return applied;
}

/**
 * Fetches the "landlock" serverStatus section from `node` and asserts its invariants: the
 * section is present by default with `enabled`/`active` booleans and a numeric `abiVersion`
 * (probed even while the option is off, so monitoring can see kernel capability: >= 1 on
 * kernels with Landlock, 0 when the kernel lacks or disables it); the sandbox is active
 * exactly when it is enabled on a kernel that supports Landlock (elsewhere the server
 * deliberately runs unsandboxed, best-effort); and the access-rights breakdowns describe the
 * enforced ruleset, so they are present exactly while active, with a never-empty handled set
 * (an unenforceable ruleset means the sandbox does not activate).
 */
function getLandlockStatus(node, desc) {
    const res = assert.commandWorked(node.adminCommand({serverStatus: 1}));
    const landlock = res.landlock;
    assert.neq(undefined, landlock, `missing landlock section in serverStatus on ${desc}`);
    assert.eq("boolean", typeof landlock.enabled, `bad landlock.enabled on ${desc}`, {landlock});
    assert.eq("boolean", typeof landlock.active, `bad landlock.active on ${desc}`, {landlock});
    assert.eq("number", typeof landlock.abiVersion, `bad landlock.abiVersion on ${desc}`, {
        landlock,
    });
    assert.gte(landlock.abiVersion, 0, `bad probed ABI version on ${desc}`, {landlock});
    assert.eq(
        landlock.enabled && landlock.abiVersion >= 1,
        landlock.active,
        `enforcement does not match the option and the kernel's Landlock support on ${desc}`,
        {landlock},
    );
    if (landlock.active) {
        assert(
            Array.isArray(landlock.handledAccessRights.fs),
            `bad handledAccessRights on ${desc}`,
            {landlock},
        );
        assert.gt(landlock.handledAccessRights.fs.length, 0, `empty handled fs rights on ${desc}`, {
            landlock,
        });
        assert(
            Array.isArray(landlock.degradedAccessRights.fs),
            `bad degradedAccessRights on ${desc}`,
            {landlock},
        );
    } else {
        assert(
            !landlock.hasOwnProperty("handledAccessRights"),
            `handledAccessRights reported while inactive on ${desc}`,
            {landlock},
        );
        assert(
            !landlock.hasOwnProperty("degradedAccessRights"),
            `degradedAccessRights reported while inactive on ${desc}`,
            {landlock},
        );
    }
    jsTest.log.info(`landlock serverStatus on ${desc}`, {landlock});
    return landlock;
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
let kernelEnforces;

describe("standalone mongod under the Landlock sandbox", function () {
    let conn;

    before(function () {
        conn = MongoRunner.runMongod(kNodeOpts);
        assert.neq(null, conn, "mongod failed to start with --landlock");
    });

    it("reaches a clean sandboxing decision", function () {
        kernelEnforces = assertSandboxOutcome(conn);
        jsTest.log.info("Landlock sandbox status on this kernel", {enforced: kernelEnforces});
    });

    // The server never exec()s files, so execution must be denied everywhere:
    // the ruleset handles EXECUTE (deny by default) and no path rule may
    // re-grant it.
    it("handles but never grants EXECUTE", function () {
        if (!kernelEnforces) {
            jsTest.log.info("kernel does not enforce Landlock; nothing to assert");
            return;
        }
        const kExecute = "LANDLOCK_ACCESS_FS_EXECUTE";

        const [ruleset] = logEntries(conn, kIdRulesetApplied);
        assert(ruleset.attr.handledAccessRights.fs.includes(kExecute), "EXECUTE is not handled", {
            ruleset,
        });

        const rules = logEntries(conn, kIdRuleApplied);
        assert.gt(rules.length, 0, "no Landlock path rules were logged");
        for (const rule of rules) {
            assert(!rule.attr.allowedAccess.includes(kExecute), "a rule granted EXECUTE", {rule});
        }
    });

    it("reports the sandbox state in serverStatus", function () {
        const landlock = getLandlockStatus(conn, "standalone mongod with --landlock");
        assert.eq(true, landlock.enabled, "landlock option not reported enabled", {landlock});
        // The section must agree with the enforcement outcome derived from the startup log.
        assert.eq(kernelEnforces, landlock.active, "serverStatus and the startup log disagree", {
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
        MongoRunner.stopMongod(conn);
    });
});

describe("mongod without --landlock", function () {
    it("reports disabled and the probed ABI version in serverStatus", function () {
        const conn = MongoRunner.runMongod({});
        try {
            const landlock = getLandlockStatus(conn, "mongod without --landlock");
            assert.eq(false, landlock.enabled, "landlock should default to disabled (POC)", {
                landlock,
            });
        } finally {
            MongoRunner.stopMongod(conn);
        }
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
            assert.eq(kernelEnforces, assertSandboxOutcome(node), "mongod outcome diverged", {
                host: node.host,
            });
        }
        assert.eq(kernelEnforces, assertSandboxOutcome(st.s), "mongos outcome diverged", {
            host: st.s.host,
        });
    });

    it("reports the sandbox state in serverStatus on every node", function () {
        for (const [node, desc] of [
            [st.s, "mongos"],
            [st.rs0.getPrimary(), "shard0 primary"],
            [st.configRS.getPrimary(), "config server primary"],
        ]) {
            const landlock = getLandlockStatus(node, `${desc} with --landlock`);
            assert.eq(true, landlock.enabled, "landlock option not reported enabled", {
                desc,
                landlock,
            });
            assert.eq(
                kernelEnforces,
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
        st.stop();
    });
});
