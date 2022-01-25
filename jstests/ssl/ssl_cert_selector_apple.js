/**
 * Validate that the server can load certificates from the
 * Secure Transport certificate store.
 *
 * Don't actually try to connect via SSL, because without interactivity,
 * we won't be able to click on the "Allow" button that Apple insists on presenting.
 *
 * Just verify that we can startup when we select a valid cert,
 * and fail when we do not.
 */

load('jstests/ssl/libs/ssl_helpers.js');

requireSSLProvider('apple', function() {
    'use strict';

    const CLIENT =
        'C=US,ST=New York,L=New York City,O=MongoDB,OU=Kernel,CN=Trusted Kernel Test Client';
    const SERVER =
        'C=US,ST=New York,L=New York City,O=MongoDB,OU=Kernel,CN=Trusted Kernel Test Server';
    const INVALID = null;

    function getCertificateSHA1BySubject(subject) {
        clearRawMongoProgramOutput();
        // security find-certificate prints out info about certificates matching the given search
        // criteria. In this case, we use -c, matching common name, and -Z, which includes SHA-1 and
        // SHA-256 thumbprints in the output.
        assert.eq(0, runNonMongoProgram("security", "find-certificate", "-c", subject, "-Z"));
        const out = rawMongoProgramOutput();

        const kSearchStr = "SHA-1 hash: ";
        const kHashHexitLen = 40;

        const searchIdx = out.indexOf(kSearchStr);
        assert.neq(searchIdx, -1, "SHA-1 hash not found in command output!");

        return out.substr(searchIdx + searchStr.length, kHashHexitLen);
    }

    // Using the thumbprint of the certificate stored in the keychain should always work as a
    // selector.
    const trusted_server_thumbprint = getCertificateSHA1BySubject("Trusted Kernel Test Server");
    const trusted_client_thumbprint = getCertificateSHA1BySubject("Trusted Kernel Test Client");

    const expected_server_thumbprint = cat("jstests/libs/trusted-server.pem.digest.sha1");
    const expected_client_thumbprint = cat("jstests/libs/trusted-client.pem.digest.sha1");

    // If we fall into this case, our trusted certificates are not installed on the machine's
    // certificate keychain. This probably means that certificates have just been renewed, but have
    // not been installed in MacOS machines yet.
    if (expected_server_thumbprint !== trusted_server_thumbprint ||
        expected_client_thumbprint !== trusted_client_thumbprint) {
        print("****************");
        print("****************");
        print(
            "macOS host has an unexpected version of the trusted server certificate (jstests/libs/trusted-server.pem) or trusted client certificate (jstests/libs/trusted-client.pem) installed.");
        print("Expecting server thumbprint: " + expected_server_thumbprint +
              ", got: " + trusted_server_thumbprint);
        print("Expecting client thumbprint: " + expected_client_thumbprint +
              ", got: " + trusted_client_thumbprint);
        print("****************");
        print("****************");
    }

    const testCases = [
        {selector: 'thumbprint=' + trusted_server_thumbprint, name: SERVER},
        {selector: 'subject=Trusted Kernel Test Server', name: SERVER},
        {selector: 'thumbprint=' + trusted_client_thumbprint, name: CLIENT},
        {selector: 'subject=Trusted Kernel Test Client', name: CLIENT},
        {selector: 'thumbprint=DEADBEEFDEADBEEFDEADBEEFDEADBEEFDEADBEEF', name: INVALID},
        {selector: 'subject=Unknown Test Client', name: INVALID}
    ];

    function test(cert, cluster) {
        const opts = {
            sslMode: 'requireSSL',
            sslCertificateSelector: cert.selector,
            sslClusterCertificateSelector: cluster.selector,
            waitForConnect: false,
            setParameter: {logLevel: '1'},
        };
        clearRawMongoProgramOutput();
        const mongod = MongoRunner.runMongod(opts);

        assert.soon(function() {
            const log = rawMongoProgramOutput();
            if ((cert.name === null) || (cluster.name === null)) {
                // Invalid search criteria should fail.
                return log.search('Certificate selector returned no results') >= 0;
            }
            // Valid search criteria should show our Subject Names.
            const certOK = log.search('\"name\":\"' + cert.name) >= 0;
            const clusOK = log.search('\"name\":\"' + cluster.name) >= 0;
            return certOK && clusOK;
        }, "Starting Mongod with " + tojson(opts), 60000);

        try {
            MongoRunner.stopMongod(mongod);
        } catch (e) {
            // Depending on timing, exitCode might be 0, 1, or -9.
            // All that matters is that it dies, resmoke will tell us if that failed.
            // So just let it go, the exit code never bothered us anyway.
        }
    }

    testCases.forEach(cert => {
        testCases.forEach(cluster => {
            test(cert, cluster);
        });
    });
});
