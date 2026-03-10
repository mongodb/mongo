/**
 * Tests that the proxy server correctly sends tlv data to a mongod.
 * @tags: [
 *   multiversion_incompatible,
 *   grpc_incompatible,
 * ]
 */

if (_isWindows()) {
    quit();
}

import {ProxyProtocolServer} from "jstests/sharding/libs/proxy_protocol.js";

const kProxyIngressPort = allocatePort();
const kProxyEgressPort = allocatePort();
const kProxyVersion = 2;

let proxyServer = new ProxyProtocolServer(kProxyIngressPort, kProxyEgressPort, kProxyVersion);
proxyServer.start();

let mongod = MongoRunner.runMongod({proxyPort: kProxyEgressPort});
const admin = mongod.getDB("admin");

// Enable the isConnectedToProxyUnixSocketOverride failpoint so that the proxy protocol header
// parsing logic treats this TCP connection as if it arrived on a proxy unix socket. TLV parsing
// happens only for such connections.
assert.commandWorked(
    admin.adminCommand({configureFailPoint: "isConnectedToProxyUnixSocketOverride", mode: "alwaysOn"}),
);

// Increase network log verbosity so we can see log 11978400 emitted.
assert.commandWorked(
    admin.runCommand({
        setParameter: 1,
        logComponentVerbosity: {network: {verbosity: 4}},
    }),
);

// The subject DN from client_roles.pem in RFC 4514 format (which parseDN expects).
const kDN = "CN=Kernel Client Peer Role,OU=Kernel Users,O=MongoDB,L=New York City,ST=New York,C=US";

// DER-encoded roles from client_roles.pem: backup@admin, readAnyDatabase@admin.
// This is the value of the mongodbRoles extension (OID 1.3.6.1.4.1.34601.2.1.1).
const kRolesDer = "\x31\x2b\x30\x0f\x0c\x06backup\x0c\x05admin\x30\x18\x0c\x0freadAnyDatabase\x0c\x05admin";

const kSNI = "my.mongodb.com";

function isEmpty(obj) {
    return obj === null || typeof obj === "undefined" || Object.keys(obj).length === 0;
}

function buildTLVString(tlvs) {
    let tlvString = "";
    if (isEmpty(tlvs)) {
        return tlvString;
    }

    for (const obj of tlvs) {
        tlvString += "0x";
        tlvString += obj["type"].toString(16).padStart(2, "0");
        tlvString += ":";
        tlvString += obj["value"];
        tlvString += ",";
    }

    // Remove the last comma.
    return tlvString.slice(0, -1);
}

// tlvs is expected to be an array of TLV objects and sslTlv is expected to be an SSL TLV object.
// See setTLVs docstring in jstests/sharding/libs/proxy_protocol.js for formart of these objects.
function runTest(tlvs, sslTlv, expectedSuccess) {
    // The setTLVs function requires tlvs and sslTlvs to be in a single array so merge them.
    let merged = [...tlvs];
    if (!isEmpty(sslTlv)) {
        merged.push(sslTlv);
    }
    proxyServer.setTLVs(merged);

    // Connecting to the proxy will open a connection to the server with a proxy protcol header.
    const uri = `mongodb://127.0.0.1:${kProxyIngressPort}`;
    if (!expectedSuccess) {
        assert.throws(() => new Mongo(uri));
        return;
    }
    new Mongo(uri);

    let tlvString = buildTLVString(tlvs);
    let sslTlvString = buildTLVString(sslTlv["ssl"] || []);

    // Verify that log line 11978400 is emitted once with the expected data after at most 30 seconds.
    checkLog.containsRelaxedJson(
        mongod,
        11978400,
        {
            "tlvs": `${tlvString}`,
            "sslTlvs": `${sslTlvString}`,
        },
        1,
        30 * 1000,
    );
}

jsTest.log.info("Test 1: Authority TLV with DN and roles in SSL sub-TLVs");
runTest(
    [{"type": 0x02, "value": "authority.example.com"}],
    {
        "ssl": [
            {"type": 0xe0, "value": kDN},
            {
                "type": 0xe1,
                "value": kRolesDer,
            },
        ],
    },
    true,
);

jsTest.log.info("Test 2: Authority TLV with other top-level TLV, no SSL sub-TLVs");
runTest(
    [
        {"type": 0x02, "value": "authority.example.com"},
        {"type": 0x05, "value": "HELLO"},
    ],
    {},
    true,
);

jsTest.log.info("Test 3: No TLVs should fail");
runTest([], {}, false);

jsTest.log.info("Test 4: Invalid TLV parsing");
runTest([{"type": 0x00, "value": "Bad"}], {}, false);
runTest([{"type": "0x01", "value": "Bad"}], {}, false);
runTest([{"types": 0x01, "value": "Bad"}], {}, false);
runTest([{"type": 0x01, "values": "Bad"}], {}, false);
runTest([{"type": 0x01, "value": "Good"}], {"ssl": {"type": 0x01, "value": "Bad"}}, false);
runTest(
    [{"type": 0x01, "value": "Good"}, {"ssl": [{"type": 0x01, "value": "Good"}]}],
    {"ssl": [{"type": 0x05, "value": "Good"}]},
    false,
);

jsTest.log.info("Test 5: DN only in SSL sub-TLV, no Authority TLV");
runTest(
    [{"type": 0x01, "value": "h2"}],
    {
        "ssl": [{"type": 0xe0, "value": kDN}],
    },
    true,
);

jsTest.log.info("Test 6: SNI + other top-level TLVs + DN + roles + version in SSL sub-TLVs");
runTest(
    [
        {"type": 0x01, "value": "h2"},
        {"type": 0x02, "value": kSNI},
        {"type": 0x05, "value": "uniqueID123"},
    ],
    {
        "ssl": [
            {"type": 0x21, "value": "TLSv1.3"},
            {"type": 0xe0, "value": kDN},
            {"type": 0xe1, "value": kRolesDer},
        ],
    },
    true,
);

jsTest.log.info("Test 7: SSL sub-TLVs with version and cipher but no DN or roles");
runTest(
    [{"type": 0x02, "value": kSNI}],
    {
        "ssl": [
            {"type": 0x21, "value": "TLSv1.3"},
            {"type": 0x23, "value": "ECDHE-RSA-AES128-GCM-SHA256"},
        ],
    },
    true,
);

jsTest.log.info("Test 8: Roles only, no DN, no SNI -- should fail");
runTest(
    [{"type": 0x01, "value": "h2"}],
    {
        "ssl": [{"type": 0xe1, "value": kRolesDer}],
    },
    false,
);

jsTest.log.info("Test 9: DN and roles without SNI");
runTest(
    [{"type": 0x01, "value": "h2"}],
    {
        "ssl": [
            {"type": 0xe0, "value": kDN},
            {"type": 0xe1, "value": kRolesDer},
        ],
    },
    true,
);

MongoRunner.stopMongod(mongod);
proxyServer.stop();
