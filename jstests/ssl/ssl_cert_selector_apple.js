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

    const testCases = [
        {selector: 'thumbprint=D7421F7442CA313821E19EE0509721F4D60B25A8', name: SERVER},
        {selector: 'subject=Trusted Kernel Test Server', name: SERVER},
        {selector: 'thumbprint=9CA511552F14D3FC2009D425873599BF77832238', name: CLIENT},
        {selector: 'subject=Trusted Kernel Test Client', name: CLIENT},
        {selector: 'thumbprint=D7421F7442CA313821E19EE0509721F4D60B25A9', name: INVALID},
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
            const certOK = log.search('Server Certificate Name: ' + cert.name) >= 0;
            const clusOK = log.search('Client Certificate Name: ' + cluster.name) >= 0;
            return certOK && clusOK;
        }, "Starting Mongod with " + tojson(opts), 10000);

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
