
load("jstests/client_encrypt/lib/mock_kms.js");
load('jstests/ssl/libs/ssl_helpers.js');

(function() {
    "use strict";

    const mock_kms = new MockKMSServer();
    mock_kms.start();

    const randomAlgorithm = "AEAD_AES_256_CBC_HMAC_SHA_512-Random";
    const deterministicAlgorithm = "AEAD_AES_256_CBC_HMAC_SHA_512-Deterministic";

    const x509_options =
        {sslMode: "requireSSL", sslPEMKeyFile: SERVER_CERT, sslCAFile: CA_CERT, vvvvv: ""};

    const conn = MongoRunner.runMongod(x509_options);
    const unencryptedDatabase = conn.getDB("test");
    const collection = unencryptedDatabase.keystore;

    const awsKMS = {
        accessKeyId: "access",
        secretAccessKey: "secret",
        url: mock_kms.getURL(),
    };

    const clientSideFLEOptionsFail = [
        {
          kmsProviders: {
              aws: awsKMS,
          },
          schemaMap: {},
        },
        {
          kmsProviders: {
              aws: awsKMS,
          },
          keyVaultNamespace: "test.keystore",
        },
        {
          keyVaultNamespace: "test.keystore",
          schemaMap: {},
        },
    ];

    clientSideFLEOptionsFail.forEach(element => {
        assert.throws(Mongo, [conn.host, element]);
    });

    const clientSideFLEOptionsPass = [
        {
          kmsProviders: {
              aws: awsKMS,
          },
          keyVaultNamespace: "test.keystore",
          schemaMap: {},
        },
    ];

    clientSideFLEOptionsPass.forEach(element => {
        assert.doesNotThrow(() => {
            Mongo(conn.host, element);
        });
    });

    MongoRunner.stopMongod(conn);
    mock_kms.stop();
}());
