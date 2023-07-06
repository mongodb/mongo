// Attempt to verify that connections can make use of TCP_FASTOPEN
// @tags: [multiversion_incompatible, does_not_support_stepdowns]

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
            print(`tcp_fastopen not enabled: ${val}`);
            return false;
        }
        return true;
    } catch (e) {
        // File not found or unreadable, assume no TFO support.
        print(`unable to read ${procFile}`);
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
    print(`==getTfoStatus(${dbHost(db)}) => {accepted:${result.accepted},...}`);
    return result;
}

function serverSupportsTfo(db) {
    const tfoStatus = getTfoStatus(db);
    if (!tfoStatus.serverSupported) {
        print("server-side TFO unsupported");
        return false;
    }
    if (!tfoStatus.clientSupported) {
        print("client-side TFO unsupported");
        return false;
    }
    return true;
}

/** Test tcpFastOpenServer by setting the flag and connecting to it as a TFO client. */
function testTcpFastOpenServer() {
    for (let [expect, params] of [         //
             [1, {}],                      //
             [1, {tcpFastOpenServer: 1}],  //
             [0, {tcpFastOpenServer: 0}],  //
    ]) {
        print(`==Running testTcpFastOpenServer test ${JSON.stringify(params)} => ${expect}`);
        const conn = MongoRunner.runMongod({setParameter: params});
        const db = conn.getDB(jsTestName());
        if (!serverSupportsTfo(db)) {
            print("==Skipping test, the mongod server doesn't support TFO");
        } else {
            let tfoAccepts = [];
            for (let i = 0; i < 2; ++i) {
                const shellExit = runMongoProgram('mongo', db.getMongo().host, '--eval', ';');
                assert.eq(0, shellExit, "cannot connect");
                tfoAccepts.push(getTfoStatus(db).accepted);
            }
            assert.eq(tfoAccepts[1] - tfoAccepts[0], expect, "unexpected TFO triggering");
            print("==Ran test");
        }
        MongoRunner.stopMongod(conn);
    }
}

if (!hostSupportsTfo()) {
    print("==Skipping test, this host doesn't support TFO");
    quit();
}
if (isActiveBlackhole()) {
    print("==Skipping test, as this host has an active TCPFastOpenBlackhole");
    quit();
}
testTcpFastOpenServer();