/**
 * Verify the FTDC metrics for reactor threads.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */
import {verifyGetDiagnosticData} from "jstests/libs/ftdc.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function getDiagnosticData(mongos) {
    let db = mongos.getDB("admin");
    const stats = verifyGetDiagnosticData(db);
    assert(stats.hasOwnProperty("networkInterfaceStats"));
    return stats.networkInterfaceStats;
}

const numThreads = 50;
const ftdcPath = MongoRunner.toRealPath("ftdc");
const st = new ShardingTest({
    shards: 1,
    mongos: {
        s0: {setParameter: {diagnosticDataCollectionDirectoryPath: ftdcPath}},
    },
});

// Block operations after they acquire a connection.
const fp = "networkInterfaceHangCommandsAfterAcquireConn";
let res = assert.commandWorked(st.s.adminCommand({configureFailPoint: fp, mode: "alwaysOn"}));

// Run some operations and have them all blocked after acquiring a connection.
let threads = [];
for (var t = 0; t < numThreads; t++) {
    let thread = new Thread(
        function (connStr, t) {
            let conn = new Mongo(connStr);
            conn.getDB("test").runCommand({insert: "test", documents: [{counter: t}]});
        },
        st.s.host,
        t,
    );
    threads.push(thread);
    thread.start();
}

// Wait for the reactor thread to block on the fail point.
// There is only one reactor thread for each network interface, and we can block that thread on `fp`
// by establishing new egress connections. The threads defined earlier trigger establishment of
// egress connections. Once the reactor thread is blocked, the number of times the server enters
// `fp` will no longer advance.
for (var t = 1; t <= numThreads; t++) {
    assert.neq(t, numThreads, "Failed to make the reactor thread block on the fail point");
    try {
        jsTestLog(`Checking fail point for times entered: ${res.count + t} ...`);
        assert.commandWorked(
            st.s.adminCommand({
                waitForFailPoint: fp,
                timesEntered: res.count + t,
                maxTimeMS: 10000, // A large timeout to mitigate scheduling issues on slow machines.
            }),
        );
    } catch (ex) {
        assert.commandFailedWithCode(ex, ErrorCodes.MaxTimeMSExpired);
        jsTestLog("The reactor thread should be blocked now!");
        break;
    }
}

// Introduce some delay before disabling the fail point and unblocking the operation.
sleep(5000);
assert.commandWorked(st.s.adminCommand({configureFailPoint: fp, mode: "off"}));

for (var t = 0; t < numThreads; t++) {
    threads[t].join();
}

function parseHistogramKey(key, unit, hist) {
    // input: "(-inf, 123 μs)"
    // output: {lowerBound: -Infinity, upperBound: 123}
    //
    // input: "totalCount"
    // output: undefined
    //
    // unit is, e.g., "μs".
    //
    // hist is the enclosing histogram, for use in error diagnostics.
    //
    // An assertion is violated on parse failure.
    const parts = key.split(",");
    if (parts.length !== 2) {
        return; // not a range, e.g. probably is "totalCount"
    }
    let [lower, upper] = parts.map((s) => s.trim());

    let lowerBound;
    if (lower.startsWith("(")) {
        assert.eq(
            lower,
            "(-inf",
            () => `unexpected open lower bound ${lower} in histogram key ${key} in histogram: ${tojson(hist)}`,
        );
        lowerBound = -Infinity;
    } else {
        assert.eq(
            lower[0],
            "[",
            () => `unexpected interval syntax ${lower} in histogram key ${key} in histogram: ${tojson(hist)}`,
        );
        const [lowerString, unitString] = lower.slice(1).split(" ");
        assert(
            !isNaN(Number(lowerString)),
            () =>
                `expected lower bound ${lowerString} to be a number in histogram key ${key} in histogram ${tojson(hist)}`,
        );
        assert.eq(
            unitString,
            unit,
            () =>
                `unexpected unit ${unitString} (expected ${unit}) in histogram key ${key} in histogram ${tojson(hist)}`,
        );
        lowerBound = Number(lowerString);
    }

    let upperBound;
    assert.eq(
        upper[upper.length - 1],
        ")",
        () => `unexpected interval syntax ${upper} in histogram key ${key} in histogram: ${tojson(hist)}`,
    );
    if (upper === "inf)") {
        upperBound = Infinity;
    } else {
        const [upperString, unitString] = upper.slice(0, upper.length - 1).split(" ");
        assert(
            !isNaN(Number(upperString)),
            () =>
                `expected upper bound ${upperString} to be a number in histogram key ${key} in histogram ${tojson(hist)}`,
        );
        assert.eq(
            unitString,
            unit,
            () =>
                `unexpected unit ${unitString} (expected ${unit}) in histogram key ${key} in histogram ${tojson(hist)}`,
        );
        upperBound = Number(upperString);
    }

    return {lowerBound, upperBound};
}

function parseHistogram(hist, unit) {
    // input: {"(-inf, 123 μs)": {"count": 0}, "[123 μs, 456 μs)": {"count": 8}, ...}
    // output: [{lowerBound: Number, upperBound: Number, count: Number}, ...]
    //     sorted by lowerBound.
    // unit is, e.g., "μs".
    // An assertion is violated on parse failure.
    const result = [];

    Object.entries(hist).forEach(([key, value]) => {
        const parsedKey = parseHistogramKey(key, unit);
        if (parsedKey === undefined) {
            return; // not a range, e.g. probably is "totalCount"
        }
        const {lowerBound, upperBound} = parsedKey;
        result.push({lowerBound, upperBound, count: value.count});
    });

    result.sort((left, right) => {
        if (left.lowerBound < right.lowerBound) {
            return -1;
        }
        if (right.lowerBound < left.lowerBound) {
            return 1;
        }
        return 0;
    });

    assert.neq(result.length, 0, () => `histogram doesn't have any bucket: ${tojson(hist)}`);

    return result;
}

jsTestLog("Verifying FTDC metrics ...");
assert.soon(
    () => {
        const metrics = getDiagnosticData(st.s);

        // We expect at least one of the tasks scheduled on reactor threads to have a long run time.
        let longRunningTasks = 0; // Tasks with a run time > 1 sec.
        for (const instance in metrics) {
            if (!metrics[instance].hasOwnProperty("runTime")) continue; // Filter out FTDC metadata.
            const thresholdMicros = 1_000_000; // one second
            longRunningTasks += parseHistogram(metrics[instance]["runTime"], "μs")
                .filter(({lowerBound}) => lowerBound >= thresholdMicros)
                .reduce((sum, {count}) => sum + count, 0);
        }
        return longRunningTasks >= 1;
    },
    "Expected to find at least one long running task",
    30000,
);

st.stop();
