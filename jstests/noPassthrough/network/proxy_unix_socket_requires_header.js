/**
 * Verifies that connections over the proxy Unix socket are rejected without a PROXY protocol v2
 * header, and accepted when routed through the proxy protocol server that injects the header.
 * Also exercises peer credential validation, both against the server's own GID and against
 * net.proxyUnixDomainSocket.fileGroupId when that is set.
 *
 * @tags: [
 *   grpc_incompatible,
 *   requires_sharding,
 * ]
 */

if (_isWindows()) {
    quit();
}

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ProxyProtocolServer} from "jstests/sharding/libs/proxy_protocol.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {isMacOS} from "jstests/libs/server_security/os_helpers.js";

const kInternalErrorCode = 1;
const kUnauthorizedLogId = 11793400;
const kValidationFailedLogId = 11793401;

function readGids(flag, outName) {
    const outFile = `${MongoRunner.dataDir}/${outName}`;
    assert.eq(0, runNonMongoProgram("bash", "-c", `id ${flag} > '${outFile}'`));
    return cat(outFile).trim().split(/\s+/).map(Number);
}

// The server and the proxy both run as this test's user, so the peer GID the server observes is
// this process's GID unless the failpoint below overrides it.
const kServerGid = readGids("-g", "server_gid.txt")[0];

// A supplementary group can be configured as fileGroupId without the server failing to chown the
// socket, which lets us exercise a GID the server does not run as.
const kOtherGid = readGids("-G", "all_gids.txt").find((gid) => gid !== kServerGid);

function makeProxySocketPath(prefix, port) {
    return `${prefix}/proxy-mongodb-${port}.sock`;
}

function getFileGid(path) {
    const outFile = `${MongoRunner.dataDir}/socket_gid.txt`;
    const statCmd = isMacOS()
        ? `stat -f %g '${path}' > '${outFile}'`
        : `stat -c %g '${path}' > '${outFile}'`;
    assert.eq(0, runNonMongoProgram("bash", "-c", statCmd), `stat failed for ${path}`);
    return Number(cat(outFile).trim());
}

function assertConnectionFails(conn, path) {
    assert.throws(
        () => new Mongo(path),
        [],
        `Expected direct connection to proxy socket to fail: ${path}`,
    );

    assert(
        checkLog.checkContainsOnceJsonStringMatch(
            conn,
            6067900,
            "msg",
            "Error while parsing proxy protocol header",
        ),
        "Expected connection to fail because the PROXY protocol header was missing",
    );
}

// Fronts the node's proxy unix socket with a proxy server and hands its ingress URI to fn.
function withProxy(conn, proxySocketPath, version, fn) {
    const proxyServer = new ProxyProtocolServer(allocatePort(), conn.port, version, {
        egressUnixSocket: proxySocketPath,
    });
    // Headers arriving on the proxy unix socket must carry at least one TLV.
    proxyServer.setTLVs([{type: 0xe0, value: "unix-proxy"}]);
    proxyServer.start();
    try {
        fn(`mongodb://127.0.0.1:${proxyServer.getIngressPort()}`);
    } finally {
        proxyServer.stop();
    }
}

// Presents gid as the connecting proxy's GID for the duration of fn.
function withPeerGid(conn, gid, fn) {
    const fp = configureFailPoint(conn, "proxyUnixDomainSocketPeerCredentialValidationOverride", {
        data: {remoteGid: NumberInt(gid)},
    });
    try {
        fn();
    } finally {
        fp.off();
    }
}

function assertProxyConnectSucceeds(conn, proxySocketPath) {
    withProxy(conn, proxySocketPath, 2, (uri) => {
        const proxiedConn = new Mongo(uri);
        assert.commandWorked(proxiedConn.getDB("admin").runCommand({ping: 1}));
        proxiedConn.close();
    });
}

function assertProxyConnectRejected(conn, proxySocketPath, logId) {
    withProxy(conn, proxySocketPath, 2, (uri) => {
        assert.throws(() => new Mongo(uri), [], "Expected the proxy connection to be rejected");
        checkLog.containsRelaxedJson(conn, logId, {}, 1, 30 * 1000);
    });
}

// Only a v2 header is accepted, and a direct connection carrying no header at all is refused.
function testHeaderRequired(conn, prefix) {
    const proxySocketPath = makeProxySocketPath(prefix, conn.port);
    assert(fileExists(proxySocketPath), `Expected proxy socket to exist: ${proxySocketPath}`);

    assertConnectionFails(conn, proxySocketPath);

    withProxy(conn, proxySocketPath, 1, (uri) => assertConnectionFails(conn, uri));
    assertProxyConnectSucceeds(conn, proxySocketPath);
}

// Without fileGroupId, the peer must share the server's GID.
function testServerGidEnforced(conn, prefix) {
    const proxySocketPath = makeProxySocketPath(prefix, conn.port);
    assert.eq(getFileGid(proxySocketPath), kServerGid);

    assertProxyConnectSucceeds(conn, proxySocketPath);
    withPeerGid(conn, kServerGid + 1, () =>
        assertProxyConnectRejected(conn, proxySocketPath, kUnauthorizedLogId),
    );
}

// A peer whose credentials the server cannot read at all is rejected too.
function testUnreadablePeerCredentialsRejected(conn, prefix) {
    const proxySocketPath = makeProxySocketPath(prefix, conn.port);
    const fp = configureFailPoint(conn, "proxyUnixDomainSocketPeerCredentialValidationOverride", {
        data: {code: kInternalErrorCode},
    });
    try {
        assertProxyConnectRejected(conn, proxySocketPath, kValidationFailedLogId);
    } finally {
        fp.off();
    }
}

// With fileGroupId set, the peer must belong to that GID instead of the server's.
function testFileGroupIdEnforced(conn, prefix) {
    const proxySocketPath = makeProxySocketPath(prefix, conn.port);
    assert.eq(getFileGid(proxySocketPath), kOtherGid);

    assertProxyConnectRejected(conn, proxySocketPath, kUnauthorizedLogId);
    withPeerGid(conn, kOtherGid, () => assertProxyConnectSucceeds(conn, proxySocketPath));
}

// Returns freshly built options; ShardingTest merges its own setParameters into what it gets.
function proxySocketOptions(prefix, fileGroupId) {
    const options = {
        proxyUnixSocketPrefix: prefix,
        setParameter: {
            proxyProtocolTimeoutSecs: 1,
            logComponentVerbosity: {network: {verbosity: 4}},
        },
    };
    if (fileGroupId !== undefined) {
        options.proxyUnixSocketFileGroupId = fileGroupId;
    }
    return options;
}

// The prefix is kept short because a unix socket path cannot exceed 108 characters on Linux.
function makePrefix(label) {
    const prefix = `${MongoRunner.dataPath}${label}`;
    mkdir(prefix);
    return prefix;
}

function runMongod(label, fileGroupId, testFns) {
    const prefix = makePrefix(`mongod_${label}`);
    const mongod = MongoRunner.runMongod(proxySocketOptions(prefix, fileGroupId));
    try {
        testFns.forEach((testFn) => testFn(mongod, prefix));
    } finally {
        MongoRunner.stopMongod(mongod);
    }
}

function runMongos(label, fileGroupId, testFns) {
    const prefix = makePrefix(`mongos_${label}`);
    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        other: {mongosOptions: proxySocketOptions(prefix, fileGroupId)},
    });
    try {
        testFns.forEach((testFn) => testFn(st.s0, prefix));
    } finally {
        st.stop();
    }
}

for (const [topology, runTopology] of [
    ["mongod", runMongod],
    ["mongos", runMongos],
]) {
    jsTest.log.info(`Testing the proxy protocol header and the server's own GID on ${topology}`);
    runTopology("default_gid", undefined, [
        testHeaderRequired,
        testServerGidEnforced,
        testUnreadablePeerCredentialsRejected,
    ]);

    if (kOtherGid === undefined) {
        jsTest.log.info(
            `Skipping fileGroupId coverage on ${topology}: no supplementary group available`,
        );
        continue;
    }

    jsTest.log.info(`Testing fileGroupId ${kOtherGid} is enforced on ${topology}`);
    runTopology("file_group_id", kOtherGid, [testFileGroupIdEnforced]);
}
