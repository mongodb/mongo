// Make sure MongoD starts with TLS 1.0 and 1.1 disabled (except w/ old OpenSSL).

import {
    CA_CERT,
    CLIENT_CERT,
    clientSupportsTLS1_1,
    clientSupportsTLS1_2,
    clientSupportsTLS1_3,
    determineSSLProvider,
    SERVER_CERT,
    sslProviderSupportsTLS1_0
} from 'jstests/ssl/libs/ssl_helpers.js';

const supportsTLS1_0 = sslProviderSupportsTLS1_0();
const supportsTLS1_1 = clientSupportsTLS1_1();
const supportsTLS1_2 = clientSupportsTLS1_2();
const supportsTLS1_3 = clientSupportsTLS1_3();

// There will be cases where a connect is impossible,
// let the test runner clean those up.
TestData.ignoreUnterminatedProcesses = true;

function test(serverDisabledProtocols, clientDisabledProtocols, shouldStart, shouldSucceed) {
    let buildInfo = getBuildInfo().openssl.compiled;
    if (!buildInfo) {
        buildInfo = determineSSLProvider() + ' native TLS';
    }
    const configStr = tojson(serverDisabledProtocols) + '/' + tojson(clientDisabledProtocols) +
        ' at ' + buildInfo + '; shouldStart: ' + shouldStart + '; shouldSucceed: ' + shouldSucceed;

    let serverOpts = {
        tlsMode: 'allowTLS',
        tlsCertificateKeyFile: SERVER_CERT,
        tlsCAFile: CA_CERT,
        waitForConnect: true
    };
    if (serverDisabledProtocols !== null) {
        serverOpts.tlsDisabledProtocols = serverDisabledProtocols;
    }
    clearRawMongoProgramOutput();
    let mongod;
    try {
        mongod = MongoRunner.runMongod(serverOpts);
    } catch (e) {
        assert(!shouldStart, 'Failed to start mongod with ' + configStr);
        return;
    }
    assert(mongod);
    assert(shouldStart, 'Expected mongod to fail with ' + configStr);

    const host = 'localhost:' + mongod.port;
    let connection;
    try {
        connection = new Mongo(host, undefined /* encryptedDBClientCallback */, {
            tls: {
                certificateKeyFile: CLIENT_CERT,
                CAFile: CA_CERT,
                disabledProtocols: clientDisabledProtocols,
                allowInvalidCertificates: true
            }
        });
        assert.commandWorked(connection.adminCommand({connectionStatus: 1}),
                             'Running with ' + configStr);
    } catch (e) {
        assert(!shouldSucceed, 'Running with ' + configStr);
    }

    const expectLogMessage_disable_1_0_1_1 = supportsTLS1_2 && (serverDisabledProtocols === null);
    const expectLogMessage_disable_1_0 =
        !expectLogMessage_disable_1_0_1_1 && supportsTLS1_1 && (serverDisabledProtocols === null);

    assert.eq(expectLogMessage_disable_1_0,
              checkLog.checkContainsOnce(mongod, 'Automatically disabling TLS 1.0,'),
              "TLS 1.0 was/wasn't automatically disabled with " + configStr);
    assert.eq(expectLogMessage_disable_1_0_1_1,
              checkLog.checkContainsOnce(mongod, 'Automatically disabling TLS 1.0 and TLS 1.1,'),
              "TLS 1.0 and TLS 1.1 was/wasn't automatically disabled with " + configStr);

    MongoRunner.stopMongod(mongod);
}

const TLS_1_0 = 1;
const TLS_1_1 = 2;
const TLS_1_2 = 4;
const TLS_1_3 = 8;
const tlsSupport = determineSSLProvider() === 'openssl'
    ? (supportsTLS1_0 ? TLS_1_0 : 0) | (supportsTLS1_1 ? TLS_1_1 : 0) |
        (supportsTLS1_2 ? TLS_1_2 : 0) | (supportsTLS1_3 ? TLS_1_3 : 0)
    : TLS_1_0 | TLS_1_1 | TLS_1_2;
const defaultServerDisable = determineSSLProvider() === 'openssl' ? TLS_1_0 | TLS_1_1 : 0;
const defaultClientDisable =
    (supportsTLS1_2 ? TLS_1_1 : 0) | (supportsTLS1_2 || supportsTLS1_1 ? TLS_1_0 : 0);

function shouldStart(serverDisable) {
    const serverDisabledProtocols = serverDisable === null ? defaultServerDisable : serverDisable;
    // TODO:
    // SERVER-98253: ssl_manager_openssl.cpp does not check if all supported protocols are disabled
    if (determineSSLProvider() === 'openssl') {
        return true;
    }

    // there are 2 scenarios where mongod will fail to start
    // 1: all valid TLS modes are disabled
    if ((serverDisabledProtocols & (TLS_1_0 | TLS_1_1 | TLS_1_2)) ===
        (TLS_1_0 | TLS_1_1 | TLS_1_2)) {
        // 'All valid TLS modes disabled'
        // apple & windows only check for 1.0, 1.1, 1.2 and ignores 1.3
        // SERVER-98279: support tls 1.3 for windows & apple
        return false;
    }

    // 2: (apple only) if the available protocols is not a continuous range
    if (determineSSLProvider() === 'apple') {
        // ssl_manager_apple.cpp: 'Can not disable TLS 1.1 while leaving 1.0 and 1.2 enabled'
        if ((serverDisabledProtocols & (TLS_1_0 | TLS_1_1 | TLS_1_2)) === TLS_1_1) {
            return false;
        }
    }
    return true;
}

function shouldSucceed(serverDisable, clientDisable) {
    serverDisable = serverDisable === null ? defaultServerDisable : serverDisable;
    clientDisable = clientDisable === null ? defaultClientDisable : clientDisable;
    return ~serverDisable & tlsSupport & ~clientDisable;
}
function tlsFlagsToString(tlsFlags) {
    if (tlsFlags === null) {
        return null;
    } else if (tlsFlags === 0) {
        return 'none';
    } else {
        let tlsStrings = [];
        if (tlsFlags & TLS_1_0) {
            tlsStrings.push('TLS1_0');
        }
        if (tlsFlags & TLS_1_1) {
            tlsStrings.push('TLS1_1');
        }
        if (tlsFlags & TLS_1_2) {
            tlsStrings.push('TLS1_2');
        }
        if (tlsFlags & TLS_1_3) {
            tlsStrings.push('TLS1_3');
        }
        return tlsStrings.join(',');
    }
}

const serverScenarios = [
    null,
    0,
    TLS_1_0,
    TLS_1_0 | TLS_1_1,
    TLS_1_0 | TLS_1_2,
    TLS_1_0 | TLS_1_1 | TLS_1_2,
    TLS_1_0 | TLS_1_2 | TLS_1_3,
    TLS_1_0 | TLS_1_1 | TLS_1_3,
    TLS_1_0 | TLS_1_1 | TLS_1_2 | TLS_1_3,
    TLS_1_1,
    TLS_1_1 | TLS_1_2,
    TLS_1_1 | TLS_1_3,
    TLS_1_1 | TLS_1_2 | TLS_1_3
];

const clientScenarios = [null, 0, TLS_1_0, TLS_1_0 | TLS_1_1];

// TODO
// SERVER-98278: bad input for --sslDisabledProtocols are not checked for windows / openssl
if (determineSSLProvider() === 'apple') {
    test('TLS_INVALID', null, false, null);
}

serverScenarios.forEach((serverDisable) => clientScenarios.forEach((clientDisable) => {
    // SERVER-98279: support tls 1.3 for windows & apple
    // skip any tests disabling TLS 1.3 since it's not supported for those platforms yet
    if (determineSSLProvider() === 'apple' || determineSSLProvider() === 'windows') {
        if (serverDisable & TLS_1_3)
            return;
    }
    test(tlsFlagsToString(serverDisable),
         tlsFlagsToString(clientDisable),
         shouldStart(serverDisable),
         shouldSucceed(serverDisable, clientDisable));
})

);
