// Test setParameter tlsWithholdClientCertificate

import {ReplSetTest} from "jstests/libs/replsettest.js";

function testRS(opts, expectWarning) {
    const rsOpts = {
        nodes: {node0: opts, node1: opts},
    };
    const rs = new ReplSetTest(rsOpts);
    rs.startSet();
    rs.initiate();
    rs.awaitReplication();

    const test = rs.getPrimary().getDB("test");
    test.foo.insert({bar: "baz"});
    rs.awaitReplication();

    function checkWarning(member) {
        const observed = /[N,n]o SSL certificate provided by peer/.test(cat(member.fullOptions.logFile));
        assert.eq(observed, expectWarning);
    }
    checkWarning(rs.getPrimary());
    checkWarning(rs.getSecondary());
    rs.stopSet();
}

const base_options = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: "jstests/libs/server.pem",
    tlsCAFile: "jstests/libs/ca.pem",
    tlsAllowInvalidHostnames: "",
    useLogFiles: true,
};
testRS(base_options, false);

const test_options = Object.extend(
    {
        tlsAllowConnectionsWithoutCertificates: "",
        setParameter: "tlsWithholdClientCertificate=true",
    },
    base_options,
);

testRS(test_options, true);

const depr_options = Object.extend(
    {
        tlsAllowConnectionsWithoutCertificates: "",
        setParameter: "tlsWithholdClientCertificate=true",
    },
    base_options,
);

testRS(depr_options, true);
