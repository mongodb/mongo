
import {CA_CERT, SERVER_CERT} from "jstests/ssl/libs/ssl_helpers.js";

const x509_options = {
    sslMode: "requireSSL",
    sslPEMKeyFile: SERVER_CERT,
    sslCAFile: CA_CERT,
    vvvvv: ""
};

const conn = MongoRunner.runMongod(x509_options);
const localKMS = {
    key: BinData(
        0,
        "tu9jUCBqZdwCelwE/EAm/4WqdxrSMi04B8e9uAV+m30rI1J2nhKZZtQjdvsSCwuI4erR6IEcEK+5eGUAODv43NDNIR9QheT2edWFewUfHKsl9cnzTc86meIzOmYl6drp"),
};

const clientSideFLEOptionsFail = [
    {
        kmsProviders: {
            local: localKMS,
        },
        schemaMap: {},
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
            local: localKMS,
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
