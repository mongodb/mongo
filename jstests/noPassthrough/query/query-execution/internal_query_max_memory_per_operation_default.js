/**
 * Verifies that the 'internalQueryMaxMemoryUsageBytesPerOperation' knob defaults to
 * max(1GB, 20% of the memory available to the process), and that a user can override it both at
 * startup and at runtime.
 */

const conn = MongoRunner.runMongod({});
assert.neq(conn, null, "mongod failed to start");
const admin = conn.getDB("admin");

const kOneGB = 1024 * 1024 * 1024;

// Determine the memory the server sees (respecting cgroup/container limits).
const hostInfo = assert.commandWorked(admin.hostInfo());
const memLimitBytes = hostInfo.system.memLimitMB * 1024 * 1024;
const twentyPercent = Math.floor(memLimitBytes / 5);
const expectedDefault = Math.max(kOneGB, twentyPercent);

// The default must be the larger of 1GB and 20% of available memory.
const defaultValue = assert.commandWorked(
    admin.runCommand({getParameter: 1, internalQueryMaxMemoryUsageBytesPerOperation: 1}),
).internalQueryMaxMemoryUsageBytesPerOperation;

// hostInfo reports memory rounded to MB while the server computes the percentage from the exact
// byte count, so allow up to ~1MB of slack for rounding.
assert.lte(
    Math.abs(defaultValue - expectedDefault),
    2 * 1024 * 1024,
    `default ${defaultValue} should be ~max(1GB, 20% of ${memLimitBytes}) = ${expectedDefault}`,
);
assert.gte(defaultValue, kOneGB, "default should never drop below 1GB");

// A user can override the value at runtime.
const override = 500 * 1024 * 1024;
assert.commandWorked(
    admin.runCommand({setParameter: 1, internalQueryMaxMemoryUsageBytesPerOperation: override}),
);
assert.eq(
    override,
    assert.commandWorked(
        admin.runCommand({getParameter: 1, internalQueryMaxMemoryUsageBytesPerOperation: 1}),
    ).internalQueryMaxMemoryUsageBytesPerOperation,
);

MongoRunner.stopMongod(conn);

// A user can also override the value at startup.
const startupOverride = 42 * 1024 * 1024;
const conn2 = MongoRunner.runMongod({
    setParameter: {internalQueryMaxMemoryUsageBytesPerOperation: startupOverride},
});
assert.neq(conn2, null, "mongod failed to start with startup override");
assert.eq(
    startupOverride,
    assert.commandWorked(
        conn2
            .getDB("admin")
            .runCommand({getParameter: 1, internalQueryMaxMemoryUsageBytesPerOperation: 1}),
    ).internalQueryMaxMemoryUsageBytesPerOperation,
);
MongoRunner.stopMongod(conn2);
