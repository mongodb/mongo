/**
 * This test checks if mongod correctly crash on startup on linux 6.19 with
 * tcmalloc per-CPU cache.
 *
 * @tags: [requires_kernel_7014]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const gracefulExitLogID = 12257600;
const findGracefulExitLogLine = new RegExp(`"id":${gracefulExitLogID}`);

function checkGracefulExitPassesOnCompatible(conn) {
    const exitCode = waitProgram(conn.pid);
    assert.eq(exitCode, 0, `Expected server to exit with code 0, got ${exitCode}`);
    assert(
        rawMongoProgramOutput(".*").search(findGracefulExitLogLine) === -1,
        `Unexpected fatal log message with ID ${gracefulExitLogID} in server output`,
    );
}

function testMongodPerCPUCacheEnabled() {
    clearRawMongoProgramOutput();
    const conn = MongoRunner.runMongod({
        env: {GLIBC_TUNABLES: "glibc.pthread.rseq=0"},
        waitForConnect: false,
        setParameter: {
            "failpoint.shutdownAtStartup": '{mode:"alwaysOn"}',
        },
    });
    checkGracefulExitPassesOnCompatible(conn);
}

function testMongosPerCPUCacheEnabled() {
    const configRS = new ReplSetTest({nodes: 1});
    configRS.startSet({configsvr: "", env: {GLIBC_TUNABLES: "glibc.pthread.rseq=1"}});
    configRS.initiate();
    clearRawMongoProgramOutput();
    const conn = MongoRunner.runMongos({
        configdb: configRS.getURL(),
        env: {GLIBC_TUNABLES: "glibc.pthread.rseq=0"},
        waitForConnect: false,
        setParameter: {
            "failpoint.shutdownAtStartup": '{mode:"alwaysOn"}',
        },
    });
    checkGracefulExitPassesOnCompatible(conn);
    configRS.stopSet();
}

function testMongodPerCPUCacheDisabled() {
    clearRawMongoProgramOutput();
    const conn = MongoRunner.runMongod({
        env: {GLIBC_TUNABLES: "glibc.pthread.rseq=1"},
        waitForConnect: false,
        setParameter: {
            "failpoint.shutdownAtStartup": '{mode:"alwaysOn"}',
        },
    });
    checkGracefulExitPassesOnCompatible(conn);
}

function testMongosPerCPUCacheDisabled() {
    const configRS = new ReplSetTest({nodes: 1});
    configRS.startSet({configsvr: "", env: {GLIBC_TUNABLES: "glibc.pthread.rseq=1"}});
    configRS.initiate();
    clearRawMongoProgramOutput();
    const conn = MongoRunner.runMongos({
        configdb: configRS.getURL(),
        env: {GLIBC_TUNABLES: "glibc.pthread.rseq=1"},
        waitForConnect: false,
        setParameter: {
            "failpoint.shutdownAtStartup": '{mode:"alwaysOn"}',
        },
    });
    checkGracefulExitPassesOnCompatible(conn);
    configRS.stopSet();
}

testMongodPerCPUCacheEnabled();
testMongosPerCPUCacheEnabled();
testMongodPerCPUCacheDisabled();
testMongosPerCPUCacheDisabled();
