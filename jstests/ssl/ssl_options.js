// Test redaction of passwords in command line SSL option parsing.

import {requireSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";

requireSSLProvider('openssl', function() {
    const baseName = "jstests_ssl_ssl_options";

    jsTest.log("Testing censorship of ssl options");

    const mongodConfig = {
        tlsCertificateKeyFile: "jstests/libs/password_protected.pem",
        tlsMode: "requireTLS",
        tlsCertificateKeyFilePassword: "qwerty",
        tlsClusterPassword: "qwerty",
        tlsCAFile: "jstests/libs/ca.pem"
    };
    const mongodSource = MongoRunner.runMongod(mongodConfig);

    const getCmdLineOptsResult = mongodSource.adminCommand("getCmdLineOpts");

    let i;
    let isPassword = false;
    for (i = 0; i < getCmdLineOptsResult.argv.length; i++) {
        if (isPassword) {
            assert.eq(getCmdLineOptsResult.argv[i],
                      "<password>",
                      "Password not properly censored: " + tojson(getCmdLineOptsResult));
            isPassword = false;
            continue;
        }

        if (getCmdLineOptsResult.argv[i] === "--tlsPEMKeyPassword" ||
            getCmdLineOptsResult.argv[i] === "--tlsClusterPassword") {
            isPassword = true;
        }
    }

    assert.eq(getCmdLineOptsResult.parsed.net.tls.certificateKeyFilePassword,
              "<password>",
              "Password not properly censored: " + tojson(getCmdLineOptsResult));
    assert.eq(getCmdLineOptsResult.parsed.net.tls.clusterPassword,
              "<password>",
              "Password not properly censored: " + tojson(getCmdLineOptsResult));

    MongoRunner.stopMongod(mongodSource);

    print(baseName + " succeeded.");
});
