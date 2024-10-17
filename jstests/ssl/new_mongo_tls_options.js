// Tests new mongo using TLS options as parameters
import {
    CA_CERT,
    CLIENT_CERT,
    SERVER_CERT,
    TRUSTED_CA_CERT,
} from "jstests/ssl/libs/ssl_helpers.js";

const mongod = MongoRunner.runMongod(
    {tlsMode: "requireTLS", tlsCertificateKeyFile: SERVER_CERT, tlsCAFile: CA_CERT});

assert.commandWorked(mongod.adminCommand({connectionStatus: 1}));

const host = "localhost:" + mongod.port;

// Succesfully connect using valid TLS parameters.
const conn1 = new Mongo(host, undefined, {tls: {certificateKeyFile: CLIENT_CERT, CAFile: CA_CERT}});
assert.commandWorked(conn1.adminCommand({connectionStatus: 1}));

// Succesfully connect using invalid CAFile but allowInvalidCertificates=true.
const conn2 = new Mongo(host, undefined, {
    tls: {certificateKeyFile: CLIENT_CERT, CAFile: TRUSTED_CA_CERT, allowInvalidCertificates: true}
});
assert.commandWorked(conn2.adminCommand({connectionStatus: 1}));

// Set allowInvalidCertificates=false and expect a failure.
assert.throwsWithCode(() => {
    new Mongo(host, undefined, {
        tls: {
            certificateKeyFile: CLIENT_CERT,
            CAFile: TRUSTED_CA_CERT,
            allowInvalidCertificates: false
        }
    });
}, ErrorCodes.HostUnreachable);

MongoRunner.stopMongod(mongod);
