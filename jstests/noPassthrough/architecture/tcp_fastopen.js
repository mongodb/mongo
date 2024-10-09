// Attempt to verify that connections can make use of TCP_FASTOPEN
// @tags: [multiversion_incompatible, does_not_support_stepdowns, grpc_incompatible]

import {getNetStatObj} from "jstests/libs/netstat.js";

function dbHost(x) {
    return x.getMongo().host;
}

// Does it make sense to expect TFO support?
function hostSupportsTfo() {
    const procFile = "/proc/sys/net/ipv4/tcp_fastopen";
    try {
        // Both client and server bits must be set to run this test.
        const val = cat(procFile);
        if ((Number.parseInt(val) & 3) != 3) {
            jsTestLog(`tcp_fastopen not enabled: ${val}`);
            return false;
        }
        return true;
    } catch (e) {
        // File not found or unreadable, assume no TFO support.
        jsTestLog(`unable to read ${procFile}`);
        return false;
    }
}

// We skip the test if TCPFastOpenBlackhole is present and > 0,
function isActiveBlackhole() {
    const blackHole = getNetStatObj().TcpExt.TCPFastOpenBlackhole;
    return blackHole && blackHole > 0;
}

function getTfoStatus(db) {
    let result = db.serverStatus().network.tcpFastOpen;
    jsTestLog(`==getTfoStatus(${dbHost(db)}) => {accepted:${result.accepted},...}`);
    return result;
}

function serverSupportsTfo(db) {
    const tfoStatus = getTfoStatus(db);
    if (!tfoStatus.serverSupported) {
        jsTestLog("server-side TFO unsupported");
        return false;
    }
    if (!tfoStatus.clientSupported) {
        jsTestLog("client-side TFO unsupported");
        return false;
    }
    return true;
}

/** Test tcpFastOpenServer by setting the flag and connecting to it as a TFO client. */
function testTcpFastOpenServer() {
    let exitEarly = false;
    for (let [expect, params] of [         //
             [1, {}],                      //
             [1, {tcpFastOpenServer: 1}],  //
             [0, {tcpFastOpenServer: 0}],  //
    ]) {
        jsTestLog(`==Running testTcpFastOpenServer test ${JSON.stringify(params)} => ${expect}`);
        const conn = MongoRunner.runMongod({setParameter: params});
        const db = conn.getDB(jsTestName());
        exitEarly = !serverSupportsTfo(db);
        if (exitEarly) {
            jsTestLog("==Skipping test, the mongod server doesn't support TFO");
        } else {
            let tfoStatusRecords = [];
            // We connect twice and observe how the
            // "network.tcpFastOpen.accepted" counter changes. The first
            // connection is a warmup to exchange TFO cookies.
            for (let i = 0; i < 2; ++i) {
                const shellExit = runMongoProgram('mongo', db.getMongo().host, '--eval', ';');
                assert.eq(0, shellExit, "cannot connect");
                tfoStatusRecords.push(getTfoStatus(db));
            }
            let lastAcceptedCount = tfoStatusRecords[tfoStatusRecords.length - 1].accepted -
                tfoStatusRecords[tfoStatusRecords.length - 2].accepted;
            if (lastAcceptedCount < expect) {
                jsTestLog(
                    "We find that in practice we sometimes do not get TFO when we expect to. " +
                    "We are logging these events but not failing the test (See SERVER-83978). " +
                    `tfoStatusRecords: ${JSON.stringify(tfoStatusRecords)}`);
            } else {
                assert.eq(lastAcceptedCount, expect, "unexpected TFO triggering");
            }
            jsTestLog("==Ran test");
        }
        MongoRunner.stopMongod(conn);
        if (exitEarly) {
            // The server doesn't support TFO, we should just exit early.
            return;
        }
    }
}

if (!hostSupportsTfo()) {
    jsTestLog("==Skipping test, this host doesn't support TFO");
    quit();
}
if (isActiveBlackhole()) {
    jsTestLog("==Skipping test, as this host has an active TCPFastOpenBlackhole");
    quit();
}
testTcpFastOpenServer();
