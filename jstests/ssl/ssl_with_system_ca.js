// On OSX this test assumes that jstests/libs/trusted-ca.pem has been added as a trusted
// certificate to the login keychain of the evergreen user. See,
// https://github.com/10gen/buildslave-cookbooks/commit/af7cabe5b6e0885902ebd4902f7f974b64cc8961
// for details.
// To install trusted-ca.pem for local testing on OSX, invoke the following at a console:
//   security add-trusted-cert -d jstests/libs/trusted-ca.pem
(function() {
'use strict';

const HOST_TYPE = getBuildInfo().buildEnvironment.target_os;
if (HOST_TYPE == "windows") {
    // OpenSSL backed imports Root CA and intermediate CA
    runProgram("certutil.exe", "-addstore", "-user", "-f", "CA", "jstests\\libs\\trusted-ca.pem");

    // SChannel backed follows Windows rules and only trusts the Root store in Local Machine and
    // Current User.
    runProgram("certutil.exe", "-addstore", "-f", "Root", "jstests\\libs\\trusted-ca.pem");
}

function testWithCerts(prefix) {
    jsTest.log(
        `Testing with SSL certs $ {
            clientPem connecting to serverPem
        }`);

    // allowSSL to get a non-SSL control connection.
    const conn = MongoRunner.runMongod(
        {sslMode: 'allowSSL', sslPEMKeyFile: 'jstests/libs/' + prefix + 'server.pem'});

    let argv = [
        './mongo',
        '--ssl',
        '--port',
        conn.port,
        '--sslPEMKeyFile',
        'jstests/libs/' + prefix + 'client.pem',
        '--eval',
        ';'
    ];

    if (HOST_TYPE == "linux") {
        // On Linux we override the default path to the system CA store to point to our
        // "trusted" CA. On Windows, this CA will have been added to the user's trusted CA list
        argv.unshift("env", "SSL_CERT_FILE=jstests/libs/trusted-ca.pem");
    }

    const exitCode = runMongoProgram.apply(null, argv);
    MongoRunner.stopMongod(conn);
    return exitCode;
}

assert.neq(0, testWithCerts(''), 'Certs signed with untrusted CA');
assert.eq(0, testWithCerts('trusted-'), 'Certs signed with trusted CA');
})();
