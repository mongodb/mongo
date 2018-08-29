// Test setParameter sslWithholdClientCertificate

(function() {
    "use strict";

    function testRS(opts, expectWarning) {
        const rsOpts = {
            nodes: {node0: opts, node1: opts},
        };
        const rs = new ReplSetTest(rsOpts);
        rs.startSet();
        rs.initiate();
        rs.awaitReplication();

        const test = rs.getPrimary().getDB('test');
        test.foo.insert({bar: "baz"});
        rs.awaitReplication();

        function checkWarning(member) {
            const observed =
                /no SSL certificate provided by peer/.test(cat(member.fullOptions.logFile));
            assert.eq(observed, expectWarning);
        }
        checkWarning(rs.getPrimary());
        checkWarning(rs.getSecondary());
        rs.stopSet();
    }

    const base_options = {
        sslMode: 'requireSSL',
        sslPEMKeyFile: 'jstests/libs/server.pem',
        sslCAFile: 'jstests/libs/ca.pem',
        sslAllowInvalidHostnames: '',
        useLogFiles: true,
    };
    testRS(base_options, false);

    const test_options = Object.extend({
        sslAllowConnectionsWithoutCertificates: '',
        setParameter: 'sslWithholdClientCertificate=true',
    },
                                       base_options);

    testRS(test_options, true);
}());
