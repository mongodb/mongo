// Verifies that the network metrics that track received and sent bytes are properly updated.
const compressor = "snappy";
const conn = MongoRunner.runMongod({networkMessageCompressors: compressor});

function getNetworkMetrics(db) {
    return assert.commandWorked(db.runCommand({serverStatus: 1})).network;
}

function runTest(conn, useCompression) {
    const db = conn.getDB("admin");
    const before = getNetworkMetrics(db);

    jsTest.log(`Running test with compression: ${useCompression}`);
    jsTest.log(before);

    function shellBody() {
        assert.commandWorked(db.runCommand(
            {insert: 'test', documents: [{"database": "MongoDB", "data": "A".repeat(1024)}]}));
    }

    if (useCompression) {
        startParallelShell(
            shellBody, conn.port, false, "--networkMessageCompressors", compressor)();
    } else {
        startParallelShell(shellBody, conn.port, false)();
    }

    const after = getNetworkMetrics(db);

    jsTest.log("Finished running test");
    jsTest.log(after);

    function diff(tag) {
        return after[tag] - before[tag];
    }

    return {
        'bytesIn': diff("bytesIn"),
        'physicalBytesIn': diff("physicalBytesIn"),
        'bytesOut': diff("bytesOut"),
        'physicalBytesOut': diff("physicalBytesOut"),
    };
}

const statsWithCompression = runTest(conn, true);
jsTest.log(`Stats with compression: ${tojson(statsWithCompression)}`);

const statsWithoutCompression = runTest(conn, false);
jsTest.log(`Stats without compression: ${tojson(statsWithoutCompression)}`);

// Both `bytesIn` and `bytesOut` metrics must advance, and the advances in `bytesOut` must be
// bigger due to the size of `serverStatus` responses.
function verifyMetricsAdvanced(stats) {
    assert.gt(stats['bytesIn'], 0);
    assert.gt(stats['bytesOut'], 0);
    assert.gt(stats['physicalBytesIn'], 0);
    assert.gt(stats['physicalBytesOut'], 0);
}
verifyMetricsAdvanced(statsWithCompression);
verifyMetricsAdvanced(statsWithoutCompression);

// Logical metrics must observe bigger advances when compression is used.
assert.gt(statsWithCompression['bytesIn'], statsWithCompression['physicalBytesIn']);
assert.gt(statsWithCompression['bytesOut'], statsWithCompression['physicalBytesOut']);

// Both physical and logical metrics must equally advance without compression.
assert.eq(statsWithoutCompression['bytesIn'], statsWithoutCompression['physicalBytesIn']);
assert.eq(statsWithoutCompression['bytesOut'], statsWithoutCompression['physicalBytesOut']);

MongoRunner.stopMongod(conn);
