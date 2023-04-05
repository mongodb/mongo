/**
 * This test does full rollovers of the X509 auth for cluster membership using configurable
 * attributes and extensions. It ensures that it is possible to change the attributes and/or
 * extensions used to determine cluster membership while rotating cluster certificates.
 *
 * @tags: [requires_persistence, requires_replication, requires_fcv_70]
 */

(function() {
'use strict';

load('jstests/ssl/libs/ssl_helpers.js');

if (determineSSLProvider() !== "openssl") {
    print('Skipping test, tlsClusterAuthX509 options are only available with OpenSSL');
    return;
}

/**
 * This is the path of the original certificate and the CA cert used in all configurations.
 */
// Subject: C=US, ST=New York, L=New York, O=MongoDB, OU=Kernel, CN=server
const originalDNAttributes = "O=MongoDB, OU=Kernel";
const originalCert = 'jstests/libs/server.pem';
const originalCACert = 'jstests/libs/ca.pem';
const defaultPolicyClusterAuthX509Override = {
    attributes: originalDNAttributes,
};
/**
 * This is the tlsClusterAuthX509Attributes and path of the certificate which is rolled over to
 * after switching to custom attributes.
 */
// Subject: C=US, ST=New York, L=New York City, CN=server, title=foo
const fooTitleDNAttributes = "C=US, ST=New York, L=New York City, title=foo";
const fooTitleDNCert = 'jstests/libs/server_title_foo_no_o_ou_dc.pem';
const fooTitleClusterAuthX509Override = {
    attributes: fooTitleDNAttributes,
};
/**
 * This is the DN of the certificate which is rolled over to after switching the custom attribute
 * value.
 */
// Subject: C=US, ST=New York, L=New York City, O=MongoDB, OU=Kernel, CN=server, title=bar
const barTitleDNAttributes = "C=US, ST=New York, L=New York City, title=bar";
const barTitleDNCert = 'jstests/libs/server_title_bar.pem';
const barTitleClusterAuthX509Override = {
    attributes: barTitleDNAttributes,
};
/**
 * This is the path of the certificate containing the cluster membership extension set to 'foo'.
 */
const fooExtensionCert = 'jstests/ssl/libs/cluster-member-foo.pem';
const fooExtensionClusterAuthX509Override = {
    extensionValue: 'foo',
};
/**
 * This is the path of the certificate containing the cluster membership extension set to 'bar'.
 */
const barExtensionCert = 'jstests/ssl/libs/cluster-member-bar.pem';
const barExtensionClusterAuthX509Override = {
    extensionValue: 'bar',
};

const rst = new ReplSetTest({
    nodes: 3,
    waitForKeys: false,
    nodeOptions: {
        tlsMode: "preferTLS",
        clusterAuthMode: "x509",
        tlsCertificateKeyFile: originalCert,
        tlsCAFile: originalCACert,
        tlsAllowInvalidHostnames: "",
    }
});
rst.startSet();

rst.initiateWithAnyNodeAsPrimary(
    Object.extend(rst.getReplSetConfig(), {writeConcernMajorityJournalDefault: true}));

// Create a user to login as when auth is enabled later
rst.getPrimary().getDB('admin').createUser({user: 'root', pwd: 'root', roles: ['root']}, {w: 3});
rst.nodes.forEach((node) => {
    assert(node.getDB("admin").auth("root", "root"));
});

// Future connections should authenticate immediately on connecting so that replSet actions succeed.
const originalAwaitConnection = MongoRunner.awaitConnection;
MongoRunner.awaitConnection = function(args) {
    const conn = originalAwaitConnection(args);
    assert(conn.getDB('admin').auth('root', 'root'));
    return conn;
};

// This will rollover the cluster to a new config in a rolling fashion. It will return when
// there is a primary and we are able to write to it.
function rolloverConfig(newConfig) {
    function restart(node) {
        const nodeId = rst.getNodeId(node);
        rst.stop(nodeId);
        const configId = "n" + nodeId;
        rst.nodeOptions[configId] = newConfig;
        rst.start(nodeId, {remember: false}, true, true);
        rst.awaitSecondaryNodes();
    }

    rst.nodes.forEach(function(node) {
        restart(node);
    });

    assert.soon(() => {
        let primary = rst.getPrimary();
        assert.commandWorked(primary.getDB("admin").runCommand({hello: 1}));
        assert.commandWorked(primary.getDB('test').a.insert({a: 1, str: 'TESTTESTTEST'}));

        return true;
    });
}

// Scenario 1: From no tlsClusterAuthX509 to tlsClusterAuthX509Attributes.
jsTestLog("Transitioning from DC/O/OU only to DC/O/OU and custom subject DN attributes");
rolloverConfig({
    tlsMode: "preferTLS",
    clusterAuthMode: "x509",
    tlsCertificateKeyFile: originalCert,
    tlsCAFile: originalCACert,
    tlsAllowInvalidHostnames: "",
    tlsClusterAuthX509Attributes: fooTitleDNAttributes,
    setParameter: {
        tlsClusterAuthX509Override: tojson(defaultPolicyClusterAuthX509Override),
    },
});

jsTestLog("Rotating cluster member certificates to use subject DNs with custom attributes");
rolloverConfig({
    tlsMode: "preferTLS",
    clusterAuthMode: "x509",
    tlsCertificateKeyFile: fooTitleDNCert,
    tlsCAFile: originalCACert,
    tlsAllowInvalidHostnames: "",
    tlsClusterAuthX509Attributes: fooTitleDNAttributes,
    setParameter: {
        tlsClusterAuthX509Override: tojson(defaultPolicyClusterAuthX509Override),
    },
});

jsTestLog("Removing DC/O/OU policy from override");
rolloverConfig({
    tlsMode: "preferTLS",
    clusterAuthMode: "x509",
    tlsCertificateKeyFile: fooTitleDNCert,
    tlsCAFile: originalCACert,
    tlsAllowInvalidHostnames: "",
    tlsClusterAuthX509Attributes: fooTitleDNAttributes,
});

jsTestLog(
    "SUCCESS - certificate rotation from DC/O/OU certs to custom subject DN attribute-based certs");

// Scenario 2: From tlsClusterAuthX509Attributes: 'title=foo' to tlsClusterAuthX509Attributes:
// 'title=bar'
jsTestLog(
    "Transitioning from custom subject DN attributes with 'title=foo' to custom subject DN attributes with 'title=bar'");
rolloverConfig({
    tlsMode: "preferTLS",
    clusterAuthMode: "x509",
    tlsCertificateKeyFile: fooTitleDNCert,
    tlsCAFile: originalCACert,
    tlsAllowInvalidHostnames: "",
    tlsClusterAuthX509Attributes: barTitleDNAttributes,
    setParameter: {
        tlsClusterAuthX509Override: tojson(fooTitleClusterAuthX509Override),
    },
});

jsTestLog("Rotating cluster member certificates to use certificates with 'title=bar'");
rolloverConfig({
    tlsMode: "preferTLS",
    clusterAuthMode: "x509",
    tlsCertificateKeyFile: barTitleDNCert,
    tlsCAFile: originalCACert,
    tlsAllowInvalidHostnames: "",
    tlsClusterAuthX509Attributes: barTitleDNAttributes,
    setParameter: {
        tlsClusterAuthX509Override: tojson(fooTitleClusterAuthX509Override),
    },
});

jsTestLog("Removing 'title=foo' attribute from override");
rolloverConfig({
    tlsMode: "preferTLS",
    clusterAuthMode: "x509",
    tlsCertificateKeyFile: barTitleDNCert,
    tlsCAFile: originalCACert,
    tlsAllowInvalidHostnames: "",
    tlsClusterAuthX509Attributes: barTitleDNAttributes,
});

jsTestLog("SUCCESS - certificate rotation from one set of custom subject DN attributes to another");

// Scenario 3: From tlsClusterAuthX509Attributes to tlsClusterAuthX509ExtensionValue.
jsTestLog("Transitioning from custom attributes to certificate extension");
rolloverConfig({
    tlsMode: "preferTLS",
    clusterAuthMode: "x509",
    tlsCertificateKeyFile: barTitleDNCert,
    tlsCAFile: originalCACert,
    tlsAllowInvalidHostnames: "",
    tlsClusterAuthX509ExtensionValue: 'foo',
    setParameter: {
        tlsClusterAuthX509Override: tojson(barTitleClusterAuthX509Override),
    },
});

jsTestLog("Rotating cluster member certificates to use certificates with extension set to 'foo'");
rolloverConfig({
    tlsMode: "preferTLS",
    clusterAuthMode: "x509",
    tlsCertificateKeyFile: fooExtensionCert,
    tlsCAFile: originalCACert,
    tlsAllowInvalidHostnames: "",
    tlsClusterAuthX509ExtensionValue: 'foo',
    setParameter: {
        tlsClusterAuthX509Override: tojson(barTitleClusterAuthX509Override),
    },
});

jsTestLog("Removing 'title=bar' attribute from override");
rolloverConfig({
    tlsMode: "preferTLS",
    clusterAuthMode: "x509",
    tlsCertificateKeyFile: fooExtensionCert,
    tlsCAFile: originalCACert,
    tlsAllowInvalidHostnames: "",
    tlsClusterAuthX509ExtensionValue: 'foo',
});

jsTestLog(
    "SUCCESS - certificate rotation from custom subject DN attributes to custom extension value");

// Scenario 4: From tlsClusterAuthX509ExtensionValue: 'foo' to tlsClusterAuthX509ExtensionValue:
// 'bar'
jsTestLog("Transitioning from one custom extension value to another");
rolloverConfig({
    tlsMode: "preferTLS",
    clusterAuthMode: "x509",
    tlsCertificateKeyFile: fooExtensionCert,
    tlsCAFile: originalCACert,
    tlsAllowInvalidHostnames: "",
    tlsClusterAuthX509ExtensionValue: 'bar',
    setParameter: {
        tlsClusterAuthX509Override: tojson(fooExtensionClusterAuthX509Override),
    },
});

jsTestLog("Rotating cluster member certificates to use certificates with extension set to 'bar'");
rolloverConfig({
    tlsMode: "preferTLS",
    clusterAuthMode: "x509",
    tlsCertificateKeyFile: barExtensionCert,
    tlsCAFile: originalCACert,
    tlsAllowInvalidHostnames: "",
    tlsClusterAuthX509ExtensionValue: 'bar',
    setParameter: {
        tlsClusterAuthX509Override: tojson(fooExtensionClusterAuthX509Override),
    },
});

jsTestLog("Removing 'foo' extension from override");
rolloverConfig({
    tlsMode: "preferTLS",
    clusterAuthMode: "x509",
    tlsCertificateKeyFile: barExtensionCert,
    tlsCAFile: originalCACert,
    tlsAllowInvalidHostnames: "",
    tlsClusterAuthX509ExtensionValue: 'bar',
});

jsTestLog("SUCCESS - certificate rotation from one custom extension value to another");

// Scenario 5: From tlsClusterAuthExtensionValue back to default.
jsTestLog("Transitioning custom extension value to custom subject DN attributes");
rolloverConfig({
    tlsMode: "preferTLS",
    clusterAuthMode: "x509",
    tlsCertificateKeyFile: fooExtensionCert,
    tlsCAFile: originalCACert,
    tlsAllowInvalidHostnames: "",
    setParameter: {
        tlsClusterAuthX509Override: tojson(barExtensionClusterAuthX509Override),
    },
});

jsTestLog("Rotating cluster member certificates to use certificates with DC/O/OU");
rolloverConfig({
    tlsMode: "preferTLS",
    clusterAuthMode: "x509",
    tlsCertificateKeyFile: originalCert,
    tlsCAFile: originalCACert,
    tlsAllowInvalidHostnames: "",
    setParameter: {
        tlsClusterAuthX509Override: tojson(barExtensionClusterAuthX509Override),
    },
});

// This works because the original cert meets the default DC/O/OU criteria.
jsTestLog("Removing 'bar' extension from override and the custom attributes");
rolloverConfig({
    tlsMode: "preferTLS",
    clusterAuthMode: "x509",
    tlsCertificateKeyFile: originalCert,
    tlsCAFile: originalCACert,
    tlsAllowInvalidHostnames: "",
});

jsTestLog("SUCCESS - certificate rotation from extension value back to default");

rst.stopSet();
})();
