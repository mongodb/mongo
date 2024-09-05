// This prelude is run before any interactive test runner session.
try {
    const {ReplSetTest} = await import("jstests/libs/replsettest.js");
    globalThis.ReplSetTest = ReplSetTest;

    const {ShardingTest} = await import("jstests/libs/shardingtest.js");
    globalThis.ShardingTest = ShardingTest;
} catch (e) {
    // ignore all errors
}
