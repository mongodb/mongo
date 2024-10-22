import {CA_CERT, SERVER_CERT} from "jstests/ssl/libs/ssl_helpers.js";

// Neither tlsCAFile nor tlsUseSystemCA
var opts = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
};
assert.throws(() => MongoRunner.runMongod(opts),
              [],
              "MongoD started successfully with neither tlsCAFile nor tlsUseSystemCA");
assert(rawMongoProgramOutput(".*").includes(
    "The use of TLS without specifying a chain of trust is no longer supported"));
clearRawMongoProgramOutput();

// Both tlsCAFile and tlsUseSystemCA
opts = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
    setParameter: {tlsUseSystemCA: true},
};
assert.throws(() => MongoRunner.runMongod(opts),
              [],
              "MongoD started successfully with both tlsCAFile and tlsUseSystemCA");
assert(rawMongoProgramOutput(".*").includes(
    "The use of both a CA File and the System Certificate store is not supported"));
clearRawMongoProgramOutput();

// Both tlsCAFile and tlsUseSystemCA, also tlsClusterCAFile (which is OK)
opts = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
    tlsClusterCAFile: CA_CERT,
    setParameter: {tlsUseSystemCA: true},
};
assert.throws(() => MongoRunner.runMongod(opts),
              [],
              "MongoD started successfully with both tlsCAFile and tlsUseSystemCA");
assert(rawMongoProgramOutput(".*").includes(
    "The use of both a CA File and the System Certificate store is not supported"));
clearRawMongoProgramOutput();

// tlsClusterCAFile without tlsCAFile
opts = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsClusterCAFile: CA_CERT,
};
assert.throws(() => MongoRunner.runMongod(opts),
              [],
              "MongoD started successfully with tlsClusterCAFile without tlsCAFile");
assert(rawMongoProgramOutput(".*").includes(
    "Specifying a tlsClusterCAFile requires a tlsCAFile also be specified"));
clearRawMongoProgramOutput();

// tlsClusterCAFile without tlsCAFile, also tlsSystemCA (which is ignored in favor of former error)
opts = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsClusterCAFile: CA_CERT,
    setParameter: {tlsUseSystemCA: true},
};
assert.throws(() => MongoRunner.runMongod(opts),
              [],
              "MongoD started successfully with tlsClusterCAFile without tlsCAFile");
assert(rawMongoProgramOutput(".*").includes(
    "Specifying a tlsClusterCAFile requires a tlsCAFile also be specified"));