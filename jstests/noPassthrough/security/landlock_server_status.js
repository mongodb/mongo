/**
 * Verifies the "landlock" serverStatus section on mongod and mongos: it is present by default and
 * reports whether the sandbox option (security.landlock.enabled) is on, the Landlock ABI version
 * probed from the running kernel (reported even when the option is off; 0 only when the kernel
 * lacks Landlock or has it disabled), whether the sandbox is actually enforced ("active"), and --
 * only while active -- the access rights the ruleset handles and the requested rights the running
 * kernel could not restrict, grouped by rule type ("fs").
 *
 * This section is the stable surface for monitoring and tests.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const targetOs = getBuildInfo().buildEnvironment.target_os;
if (targetOs !== "linux") {
    jsTest.log.info("Landlock is Linux-only; skipping test", {targetOs});
    quit();
}

function getLandlockStatus(conn, desc) {
    const res = assert.commandWorked(conn.adminCommand({serverStatus: 1}));
    const landlock = res.landlock;
    assert.neq(undefined, landlock, `missing landlock section in serverStatus on ${desc}`);
    assert.eq("boolean", typeof landlock.enabled, `bad landlock.enabled on ${desc}`, {landlock});
    assert.eq("boolean", typeof landlock.active, `bad landlock.active on ${desc}`, {landlock});
    assert.eq("number", typeof landlock.abiVersion, `bad landlock.abiVersion on ${desc}`, {
        landlock,
    });
    assertAccessRightsShape(landlock, desc);
    jsTest.log.info(`landlock serverStatus on ${desc}`, {landlock});
    return landlock;
}

// The access-rights breakdowns describe the enforced ruleset, so they are present exactly while
// the sandbox is active. The handled set can never be empty (an unenforceable ruleset means the
// sandbox does not activate); the degraded set is empty on kernels whose ABI supports every
// requested right.
function assertAccessRightsShape(landlock, desc) {
    if (landlock.active) {
        assert.gte(landlock.abiVersion, 1, `active without kernel Landlock support on ${desc}`, {
            landlock,
        });
    }
    if (!landlock.active) {
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
        return;
    }
    assert(Array.isArray(landlock.handledAccessRights.fs), `bad handledAccessRights on ${desc}`, {
        landlock,
    });
    assert.gt(landlock.handledAccessRights.fs.length, 0, `empty handled fs rights on ${desc}`, {
        landlock,
    });
    assert(Array.isArray(landlock.degradedAccessRights.fs), `bad degradedAccessRights on ${desc}`, {
        landlock,
    });
}

describe("landlock section in serverStatus on mongod", function () {
    it("reports disabled and the probed ABI version without --landlock", function () {
        const conn = MongoRunner.runMongod({});
        try {
            const landlock = getLandlockStatus(conn, "mongod without --landlock");
            assert.eq(false, landlock.enabled, "landlock should default to disabled (POC)", {
                landlock,
            });
            assert.eq(false, landlock.active, "sandbox active without --landlock", {landlock});
            // The ABI is probed even while the sandbox option is off, so monitoring can see
            // kernel capability: >= 1 on kernels with Landlock, 0 when the kernel lacks or
            // disables it.
            assert.gte(landlock.abiVersion, 0, "bad probed ABI version while disabled", {
                landlock,
            });
        } finally {
            MongoRunner.stopMongod(conn);
        }
    });

    it("reports enabled and the probed ABI version with --landlock", function () {
        const conn = MongoRunner.runMongod({landlock: "true"});
        try {
            const landlock = getLandlockStatus(conn, "mongod with --landlock");
            assert.eq(true, landlock.enabled, "landlock option not reported enabled", {landlock});
            // The ABI version comes from the LANDLOCK_CREATE_RULESET_VERSION probe: >= 1 on
            // kernels with Landlock, 0 when the running kernel lacks or disables it.
            assert.gte(landlock.abiVersion, 0, "bad probed ABI version", {landlock});
            // TODO(SERVER-130423): flip to expect active on Landlock kernels once the
            // filesystem path policy is defined; today the sandbox stays disengaged.
            assert.eq(false, landlock.active, "sandbox unexpectedly active without a policy", {
                landlock,
            });
        } finally {
            MongoRunner.stopMongod(conn);
        }
    });
});

describe("landlock section in serverStatus on mongos", function () {
    let st;

    before(function () {
        st = new ShardingTest({
            shards: 1,
            rs: {nodes: 1},
            mongos: 1,
            other: {mongosOptions: {landlock: "true"}},
        });
    });

    after(function () {
        if (st) {
            st.stop();
        }
    });

    it("reports enabled and the probed ABI version with --landlock", function () {
        const landlock = getLandlockStatus(st.s, "mongos with --landlock");
        assert.eq(true, landlock.enabled, "landlock option not reported enabled on mongos", {
            landlock,
        });
        assert.gte(landlock.abiVersion, 0, "bad probed ABI version on mongos", {landlock});
        // TODO(SERVER-130423): flip to expect active on Landlock kernels once the
        // filesystem path policy is defined; today the sandbox stays disengaged.
        assert.eq(
            false,
            landlock.active,
            "sandbox unexpectedly active without a policy on mongos",
            {
                landlock,
            },
        );
    });

    it("reports disabled on cluster members without --landlock", function () {
        const landlock = getLandlockStatus(st.rs0.getPrimary(), "shard0 without --landlock");
        assert.eq(false, landlock.enabled, "landlock should default to disabled (POC)", {
            landlock,
        });
        assert.eq(false, landlock.active, "sandbox active without --landlock on shard0", {
            landlock,
        });
    });
});
