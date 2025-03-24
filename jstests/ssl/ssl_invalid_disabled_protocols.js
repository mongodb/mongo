import {CA_CERT, SERVER_CERT} from "jstests/ssl/libs/ssl_helpers.js";

function allDisabledProtocols() {
    jsTestLog(`All Protocols Disabled, Should Throw Error`);
    const opts = {
        tlsMode: 'requireTLS',
        tlsCertificateKeyFile: SERVER_CERT,
        tlsCAFile: CA_CERT,
        sslDisabledProtocols: "TLS1_0,TLS1_1,TLS1_2,TLS1_3"  // Disabling all TLS protocols
    };
    clearRawMongoProgramOutput();
    assert.throws(() => {
        MongoRunner.runMongod(opts);
    });
}
allDisabledProtocols();

function oneEnabledProtocol() {
    jsTestLog(`TLS1_2 Enabled, Should Pass`);
    const opts = {
        tlsMode: 'requireTLS',
        tlsCertificateKeyFile: SERVER_CERT,
        tlsCAFile: CA_CERT,
        sslDisabledProtocols: "TLS1_0,TLS1_1,TLS1_3"  // Disabling 0, 1, and 3
    };
    const mongod = MongoRunner.runMongod(opts);
    MongoRunner.stopMongod(mongod);
}
oneEnabledProtocol();
