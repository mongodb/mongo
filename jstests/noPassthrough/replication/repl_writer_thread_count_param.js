// This test ensures that the replWriterThreadCount server parameter:
//       1) cannot be less than 1
//       2) cannot be greater than 256
//       3) is actually set to the passed in value
//       4) can be altered at run time
import {ReplSetTest} from "jstests/libs/replsettest.js";

function testSettingParameter() {
    // too low a count
    clearRawMongoProgramOutput();
    const tooLowThreadCount = 0;
    assert.throws(() =>
        MongoRunner.runMongod({replSet: "rs0", setParameter: "replWriterThreadCount=" + tooLowThreadCount.toString()}),
    );
    assert(
        rawMongoProgramOutput("Invalid value for parameter replWriterThreadCount: ").match(
            tooLowThreadCount.toString() + " is not greater than or equal to 1",
        ),
        "mongod started with too low a value for replWriterThreadCount",
    );

    // too high a count
    clearRawMongoProgramOutput();
    const tooHighThreadCount = 257;
    assert.throws(() =>
        MongoRunner.runMongod({replSet: "rs0", setParameter: "replWriterThreadCount=" + tooHighThreadCount.toString()}),
    );
    assert(
        rawMongoProgramOutput("Invalid value for parameter replWriterThreadCount: ").match(
            tooHighThreadCount.toString() + " is not less than or equal to 256",
        ),
        "mongod started with too high a value for replWriterThreadCount",
    );

    // proper counts
    clearRawMongoProgramOutput();
    const acceptableMinThreadCount = 4;
    const acceptableThreadCount = 24;

    const conn = MongoRunner.runMongod({
        replSet: "rs0",
        setParameter: {
            replWriterMinThreadCount: acceptableMinThreadCount,
            replWriterThreadCount: acceptableThreadCount,
        },
    });
    assert.neq(null, conn, "mongod failed to start with a suitable replWriterThreadCount value");

    // Initialize replica set
    const adminDB = conn.getDB("admin");
    assert.commandWorked(
        adminDB.runCommand({
            replSetInitiate: {
                _id: "rs0",
                members: [{_id: 0, host: conn.host}],
            },
        }),
    );

    const hostInfoRes = adminDB.runCommand({hostInfo: 1});
    const numCores = hostInfoRes.system.numCores;
    jsTest.log.info("System info: numCores=", numCores);
    const threadCountCap = numCores * 2;

    const actualExpectedMaxThreads = Math.min(acceptableThreadCount, threadCountCap);

    assert.soon(
        () =>
            rawMongoProgramOutput("11280000.*Starting thread pool").match(
                'ReplWriterWorkerThreadPool.*"minThreads":' +
                    acceptableMinThreadCount.toString() +
                    '.*"maxThreads":' +
                    actualExpectedMaxThreads.toString(),
            ),
        "ReplWriterWorker thread pool did not start",
        // Wait up to 10 seconds for repl set initialization to finish
        10 * 1000,
    );
    assert(
        rawMongoProgramOutput("Invalid value for parameter replWriterThreadCount").length == 0,
        "despite accepting the replWriterThreadCount value, mongod logged an error",
    );
    assert(
        rawMongoProgramOutput("Invalid value for parameter replWriterMinThreadCount").length == 0,
        "despite accepting the replWriterMinThreadCount value, mongod logged an error",
    );

    // getParameter to confirm the server parameter value:
    {
        const result = adminDB.runCommand({getParameter: 1, replWriterThreadCount: 1, replWriterMinThreadCount: 1});
        assert.eq(acceptableThreadCount, result.replWriterThreadCount, "replWriterThreadCount was not set internally");
        assert.eq(
            acceptableMinThreadCount,
            result.replWriterMinThreadCount,
            "replWriterMinThreadCount was not set internally",
        );
    }

    // Make sure using setParameter with a wrong max thread count still fails:
    {
        // Can't set max to 0
        assert.commandFailed(adminDB.runCommand({setParameter: 1, replWriterThreadCount: tooLowThreadCount}));
        let result = adminDB.runCommand({getParameter: 1, replWriterThreadCount: 1});
        assert.eq(
            acceptableThreadCount,
            result.replWriterThreadCount,
            "replWriterThreadCount was overwritten by invalid value",
        );
        // Can't set max to less than min (4)
        assert.commandFailed(
            adminDB.runCommand({setParameter: 1, replWriterThreadCount: acceptableMinThreadCount - 1}),
        );
        result = adminDB.runCommand({getParameter: 1, replWriterThreadCount: 1});
        assert.eq(
            acceptableThreadCount,
            result.replWriterThreadCount,
            "replWriterThreadCount was overwritten by invalid value",
        );
        // Can't set max to >256
        assert.commandFailed(adminDB.runCommand({setParameter: 1, replWriterThreadCount: tooHighThreadCount}));
        result = adminDB.runCommand({getParameter: 1, replWriterThreadCount: 1});
        assert.eq(
            acceptableThreadCount,
            result.replWriterThreadCount,
            "replWriterThreadCount was overwritten by invalid value",
        );
    }

    // Make sure using setParameter with a wrong min thread count still fails:
    {
        // Can't set min to <0
        assert.commandFailed(adminDB.runCommand({setParameter: 1, replWriterMinThreadCount: -3}));
        let result = adminDB.runCommand({getParameter: 1, replWriterMinThreadCount: 1});
        assert.eq(
            acceptableMinThreadCount,
            result.replWriterMinThreadCount,
            "replWriterMinThreadCount was overwritten by invalid value",
        );
        // Can't set min to >max
        assert.commandFailed(
            adminDB.runCommand({setParameter: 1, replWriterMinThreadCount: acceptableThreadCount + 1}),
        );
        result = adminDB.runCommand({getParameter: 1, replWriterMinThreadCount: 1});
        assert.eq(
            acceptableMinThreadCount,
            result.replWriterMinThreadCount,
            "replWriterMinThreadCount was overwritten by invalid value",
        );
        // Can't set min to > pool size, even if max is higher
        assert.commandFailed(
            adminDB.runCommand({setParameter: 1, replWriterMinThreadCount: actualExpectedMaxThreads + 1}),
        );
        result = adminDB.runCommand({getParameter: 1, replWriterMinThreadCount: 1});
        assert.eq(
            acceptableMinThreadCount,
            result.replWriterMinThreadCount,
            "replWriterMinThreadCount was overwritten by invalid value",
        );
    }

    // decrease maximum thread count
    {
        jsTest.log.info("Decreasing max thread count");
        const lowerMax = actualExpectedMaxThreads - 1;
        // If this is not the case, we cannot continue with the test
        assert(
            lowerMax >= acceptableMinThreadCount,
            "Must have at least 3 cores available to run this test (current number: " + numCores + ")",
        );

        assert.commandWorked(adminDB.runCommand({setParameter: 1, replWriterThreadCount: lowerMax}));
        const result = adminDB.runCommand({getParameter: 1, replWriterThreadCount: 1});
        assert.eq(lowerMax, result.replWriterThreadCount, "replWriterThreadCount was not set internally");
    }

    // increase maximum thread count
    {
        jsTest.log.info("Increasing max thread count");
        assert.commandWorked(adminDB.runCommand({setParameter: 1, replWriterThreadCount: actualExpectedMaxThreads}));
        const result = adminDB.runCommand({getParameter: 1, replWriterThreadCount: 1});
        assert.eq(
            actualExpectedMaxThreads,
            result.replWriterThreadCount,
            "replWriterThreadCount was not set internally",
        );
    }

    // increase minimum thread count
    {
        jsTest.log.info("Increasing min thread count");
        const higherMin = acceptableMinThreadCount + 2;
        // we have already ensured that there are at least 3 cores.
        assert.commandWorked(adminDB.runCommand({setParameter: 1, replWriterMinThreadCount: higherMin}));
        const result = adminDB.runCommand({getParameter: 1, replWriterMinThreadCount: 1});
        assert.eq(higherMin, result.replWriterMinThreadCount, "replWriterMinThreadCount was not set internally");
    }

    // decrease minimum thread count
    {
        jsTest.log.info("Decreasing min thread count");
        const lowerMin = acceptableMinThreadCount;
        assert.commandWorked(adminDB.runCommand({setParameter: 1, replWriterMinThreadCount: lowerMin}));
        const result = adminDB.runCommand({getParameter: 1, replWriterMinThreadCount: 1});
        assert.eq(lowerMin, result.replWriterMinThreadCount, "replWriterMinThreadCount was not set internally");
    }

    // increase maximum thread count above number of cores * 2
    {
        jsTest.log.info("Increase maximum threads over number of cores * 2");
        const maxAboveActualPoolMaxSize = threadCountCap + 4;

        assert.commandWorked(adminDB.runCommand({setParameter: 1, replWriterThreadCount: maxAboveActualPoolMaxSize}));
        const result = adminDB.runCommand({getParameter: 1, replWriterThreadCount: 1});
        assert.eq(
            maxAboveActualPoolMaxSize,
            result.replWriterThreadCount,
            "replWriterThreadCount was not set internally",
        );
        const matchParameters =
            '.*"replWriterThreadCount":"' +
            maxAboveActualPoolMaxSize.toString() +
            '.*"maxThreads":"' +
            threadCountCap.toString() +
            '.*"numCores":"' +
            numCores.toString();
        jsTest.log.info("looking for " + matchParameters + " in test output");
        assert.soon(
            () =>
                rawMongoProgramOutput(
                    "11280003.*replWriterThreadCount is set to higher than the max number of threads",
                ).match(matchParameters),
            "mongod did not warn the user that the value will not take effect due to exceeding the number of cores times two",
            // Wait up to 10 seconds for the log to publish
            10 * 1000,
        );
    }

    MongoRunner.stopMongod(conn);
}

function testOplogApplication() {
    clearRawMongoProgramOutput();

    const rst = new ReplSetTest({name: jsTest.name(), nodes: 2});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();

    const secondaries = rst.getSecondaries();
    const dbName = "testDB";
    const collName = jsTest.name();
    const primaryDB = primary.getDB(dbName);
    const primaryColl = primaryDB[collName];

    const hostInfoRes = primaryDB.runCommand({hostInfo: 1});
    const numCores = hostInfoRes.system.numCores;
    jsTest.log.info("System info: numCores=", numCores);
    const twoThreadsPerCore = numCores * 2;

    jsTest.log.info("Set thread pool size to two threads per core");
    for (const secondary of secondaries) {
        assert.commandWorked(secondary.adminCommand({setParameter: 1, replWriterThreadCount: twoThreadsPerCore}));
        let result = secondary.adminCommand({getParameter: 1, replWriterThreadCount: 1});
        assert.eq(twoThreadsPerCore, result.replWriterThreadCount, "replWriterThreadCount was not set internally");
        assert.commandWorked(secondary.adminCommand({setParameter: 1, replWriterMinThreadCount: twoThreadsPerCore}));
        result = secondary.adminCommand({getParameter: 1, replWriterMinThreadCount: 1});
        assert.eq(
            twoThreadsPerCore,
            result.replWriterMinThreadCount,
            "replWriterMinThreadCount was not set internally",
        );
        secondary.adminCommand({
            setParameter: 1,
            logComponentVerbosity: {replication: {verbosity: 2}},
        });
        secondary.adminCommand({
            setParameter: 1,
            logComponentVerbosity: {executor: {verbosity: 1}},
        });
    }

    assert.commandWorked(primaryDB.createCollection(collName, {writeConcern: {w: "majority"}}));

    const numWrites = 500;
    assert.commandWorked(primaryColl.insert({_id: "writeAllDurable"}, {writeConcern: {w: 2}}));

    for (let i = 0; i < numWrites; i++) {
        assert.commandWorked(primaryColl.insert({_id: "majority2" + i}, {writeConcern: {w: 2}}));
    }

    assert(
        rawMongoProgramOutput("11280004.*Number of workers for oplog application").match(
            '.*"nWorkers":' + twoThreadsPerCore.toString(),
        ),
        "mongod did not print the correct thread pool size (2*number of cores) when applying oplog",
    );
    assert(
        rawMongoProgramOutput("Reaping this thread as we are above maxThreads").length == 0,
        "Threads were reaped unexpectedly before the pool size was modified",
    );
    clearRawMongoProgramOutput();

    jsTest.log.info("Decrease thread pool size to one thread per core");
    for (const secondary of secondaries) {
        assert.commandWorked(secondary.adminCommand({setParameter: 1, replWriterMinThreadCount: numCores}));
        let result = secondary.adminCommand({getParameter: 1, replWriterMinThreadCount: 1});
        assert.eq(numCores, result.replWriterMinThreadCount, "replWriterMinThreadCount was not set internally");
        assert.commandWorked(secondary.adminCommand({setParameter: 1, replWriterThreadCount: numCores}));
        result = secondary.adminCommand({getParameter: 1, replWriterThreadCount: 1});
        assert.eq(numCores, result.replWriterThreadCount, "replWriterThreadCount was not set internally");
    }
    // Ensure thread pool size has changed
    for (let i = numWrites; i < numWrites * 2; i++) {
        assert.commandWorked(primaryColl.insert({_id: "majority2" + i}, {writeConcern: {w: 2}}));
    }

    assert(
        rawMongoProgramOutput("11280004.*Number of workers for oplog application").match(
            '.*"nWorkers":' + numCores.toString(),
        ),
        "mongod did not print the correct thread pool size (1*number of cores) when applying oplog",
    );

    // Check that threads above numCores were reaped
    for (let i = numCores; i < twoThreadsPerCore; i++) {
        const threadNum = i + 1;
        assert(
            rawMongoProgramOutput("Reaping this thread as we are above maxThreads").match(
                '"numThreads":' + threadNum + ',"maxThreads":' + numCores,
            ),
            "the right number of threads were not reaped to meet maxThreads (" + threadNum + " was not present)",
        );
    }
    // Should not have reaped more than numCores threads
    assert(
        !rawMongoProgramOutput("Reaping this thread as we are above maxThreads").match(
            '"numThreads":' + numCores + ',"maxThreads":' + numCores,
        ),
        "the right number of threads were not reaped to meet maxThreads",
    );
    clearRawMongoProgramOutput();

    jsTest.log.info("Increase thread pool size");
    const newThreads = numCores + 2;
    for (const secondary of secondaries) {
        assert.commandWorked(secondary.adminCommand({setParameter: 1, replWriterThreadCount: newThreads}));
        let result = secondary.adminCommand({getParameter: 1, replWriterThreadCount: 1});
        assert.eq(newThreads, result.replWriterThreadCount, "replWriterThreadCount was not set internally");
        assert.commandWorked(secondary.adminCommand({setParameter: 1, replWriterMinThreadCount: newThreads}));
        result = secondary.adminCommand({getParameter: 1, replWriterMinThreadCount: 1});
        assert.eq(newThreads, result.replWriterMinThreadCount, "replWriterMinThreadCount was not set internally");
    }
    // check that new threads were spawned to meet minThreads
    for (let i = numCores; i < newThreads; i++) {
        assert(
            rawMongoProgramOutput("Spawning new thread as we are below minThreads").match(
                '"numThreads":' + i + ',"minThreads":' + newThreads,
            ),
            "the right number of threads were not spawned to meet minThreads",
        );
    }
    // Check that we did not launch any extra threads
    assert(
        !rawMongoProgramOutput("Spawning new thread as we are below minThreads").match(
            '"numThreads":' + newThreads + ',"minThreads":' + newThreads,
        ),
        "the right number of threads were not spawned to meet minThreads",
    );
    // Ensure thread pool size has changed
    for (let i = numWrites * 2; i < numWrites * 3 + 1; i++) {
        assert.commandWorked(primaryColl.insert({_id: "majority2" + i}, {writeConcern: {w: 2}}));
    }

    assert(
        rawMongoProgramOutput("11280004.*Number of workers for oplog application").match(
            '.*"nWorkers":' + newThreads.toString(),
        ),
        "mongod did not print the correct thread pool size (1*number of cores) when applying oplog",
    );
    rst.stopSet();
}

testSettingParameter();
testOplogApplication();
