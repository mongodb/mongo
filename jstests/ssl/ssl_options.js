var baseName = "jstests_ssl_ssl_options";

jsTest.log("Testing censorship of ssl options");

var mongodConfig = {
    sslPEMKeyFile: "jstests/libs/password_protected.pem",
    sslMode: "requireSSL",
    sslPEMKeyPassword: "qwerty",
    sslClusterPassword: "qwerty",
    sslCAFile: "jstests/libs/ca.pem"
};
var mongodSource = MongoRunner.runMongod(mongodConfig);

var getCmdLineOptsResult = mongodSource.adminCommand("getCmdLineOpts");

var i;
var isPassword = false;
for (i = 0; i < getCmdLineOptsResult.argv.length; i++) {
    if (isPassword) {
        assert.eq(getCmdLineOptsResult.argv[i],
                  "<password>",
                  "Password not properly censored: " + tojson(getCmdLineOptsResult));
        isPassword = false;
        continue;
    }

    if (getCmdLineOptsResult.argv[i] === "--sslPEMKeyPassword" ||
        getCmdLineOptsResult.argv[i] === "--sslClusterPassword") {
        isPassword = true;
    }
}
assert.eq(getCmdLineOptsResult.parsed.net.ssl.PEMKeyPassword,
          "<password>",
          "Password not properly censored: " + tojson(getCmdLineOptsResult));
assert.eq(getCmdLineOptsResult.parsed.net.ssl.clusterPassword,
          "<password>",
          "Password not properly censored: " + tojson(getCmdLineOptsResult));

MongoRunner.stopMongod(mongodSource.port);

print(baseName + " succeeded.");
