/**
 * This test checks that tlsClusterAuthX509Attributes can be set in appropriate scenarios
 * to specify the X.509 subject attributes that should be matched to consider a connectin client
 * as a peer server node.
 *
 * @tags: [featureFlagConfigurableX509ClusterAuthn]
 */

(function() {
'use strict';

load('jstests/ssl/libs/ssl_helpers.js');

if (determineSSLProvider() !== "openssl") {
    print('Skipping test, tlsClusterAuthX509 options are only available with OpenSSL');
    return;
}

const clusterMembershipAttributesDN = "title=foo, C=US, ST=New York, L=New York City";
const clusterMembershipOverrideDN =
    "C=US, ST=New York, L=New York, O=MongoDB Inc. (Rollover), OU=Kernel (Rollover), CN=server";

/**
 * Member certificates whose subjects include OU, O and some attributes matched by
 * tlsClusterAuthX509Attributes.
 */
// Subject: CN=server, title=foo, C=US, ST=New York, L=New York City, O=MongoDB, OU=Kernel
const serverTitleFooCert = 'jstests/libs/server_title_foo.pem';
// Subject: CN=clusterTest, title=foo, C=US, ST=New York, L=New York City, O=MongoDB, OU=Kernel
const clusterTitleFooCert = 'jstests/libs/cluster_title_foo.pem';

/**
 * Member certificates whose subjects do not include DC, OU, or O.
 */
// Subject: CN=server, title=foo, C=US, ST=New York, L=New York City
const serverTitleFooNoDefaultCert = 'jstests/libs/server_title_foo_no_o_ou_dc.pem';
// Subject: CN=clusterTest, title=foo, C=US, ST=New York, L=New York City
const clusterTitleFooNoDefaultCert = 'jstests/libs/cluster_title_foo_no_o_ou_dc.pem';

/**
 * Certificates that will not satisfy clusterMembershipAttributesDN.
 */
// Subject: CN=server, title=bar, C=US, ST=New York, L=New York City, O=MongoDB, OU=Kernel
const serverTitleBarCert = 'jstests/libs/server_title_bar.pem';
// Subject: CN=server, C=US, ST=New York, L=New York City, O=MongoDB, OU=Kernel
const serverDefaultOnlyCert = 'jstests/libs/server.pem';
// Subject: CN=clusterTest, C=US, ST=New York, L=New York City, O=MongoDB, OU=Kernel
const clusterDefaultOnlyCert = 'jstests/libs/cluster_cert.pem';

const serverCAFile = 'jstests/libs/ca.pem';

function assertNoStart(opts, errmsg) {
    clearRawMongoProgramOutput();
    assert.throws(() => MongoRunner.runMongod(opts));
    assert(rawMongoProgramOutput().includes(errmsg));
}

function checkInvalidConfigurations() {
    // Check that the option cannot be set unless clusterAuthMode == 'x509'.
    const invalidClusterAuthModeOpts = {
        auth: '',
        tlsClusterAuthX509Attributes: clusterMembershipAttributesDN
    };
    jsTest.log('No clusterAuthMode set');
    assertNoStart(
        invalidClusterAuthModeOpts,
        'Cannot set clusterAuthX509.attributes when clusterAuthMode does not allow X.509');

    jsTest.log('clusterAuthMode == keyFile');
    invalidClusterAuthModeOpts.clusterAuthMode = 'keyFile';
    assertNoStart(
        invalidClusterAuthModeOpts,
        'Cannot set clusterAuthX509.attributes when clusterAuthMode does not allow X.509');

    // Check that the server fails to start if both tlsClusterAuthX509Attributes and
    // tlsX509ClusterAuthDNOverride are set.
    const invalidTlsX509ClusterAuthDNOverrideOpts = {
        auth: '',
        tlsClusterAuthX509Attributes: clusterMembershipAttributesDN,
        clusterAuthMode: 'x509',
        tlsMode: 'preferTLS',
        setParameter: {
            tlsX509ClusterAuthDNOverride: clusterMembershipOverrideDN,
        },
        tlsCertificateKeyFile: serverTitleFooCert,
        tlsCAFile: serverCAFile,
        tlsClusterFile: clusterTitleFooCert,
    };
    jsTest.log('tlsX509ClusterAuthDNOverride also set');
    assertNoStart(
        invalidTlsX509ClusterAuthDNOverrideOpts,
        'tlsClusterAuthX509Attributes and tlsX509ClusterAuthDNOverride cannot both be set at once');

    // Check that the server fails to start if both tlsClusterAuthX509Attributes and
    // tlsClusterAuthX509ExtensionValue are set.
    const invalidClusterAuthX509ExtensionValOpts = {
        auth: '',
        tlsClusterAuthX509Attributes: clusterMembershipAttributesDN,
        tlsClusterAuthX509ExtensionValue: 'foo',
        clusterAuthMode: 'x509',
        tlsMode: 'preferTLS',
        tlsCertificateKeyFile: serverTitleFooCert,
        tlsCAFile: serverCAFile,
        tlsClusterFile: clusterTitleFooCert,
    };
    jsTest.log('tlsClusterAuthX509ExtensionValue also set');
    assertNoStart(
        invalidClusterAuthX509ExtensionValOpts,
        'net.tls.clusterAuthX509.attributes is not allowed when net.tls.clusterAuthX509.extensionValue is specified');

    // Check that the server fails to start if the provided tlsClusterFile or tlsCertificateKeyFile
    // do not contain the attributes + values specified by the tlsClusterAuthX509Attributes option.
    // This ensures consistency between the member certificates provided to cluster nodes and the
    // attributes they will be matching on.
    const mismatchedTlsCertificateKeyFileOpts = {
        auth: '',
        tlsClusterAuthX509Attributes: clusterMembershipAttributesDN,
        clusterAuthMode: 'x509',
        tlsMode: 'preferTLS',
        tlsCertificateKeyFile: serverDefaultOnlyCert,
        tlsCAFile: serverCAFile,
        tlsClusterFile: clusterDefaultOnlyCert,
    };
    jsTest.log('Mismatched tlsCertificateKeyFile');
    assertNoStart(
        mismatchedTlsCertificateKeyFileOpts,
        "The server's outgoing certificate's DN does not contain the attributes specified in tlsClusterAuthX509Attributes");
}

function authX509(expectedUsername, port, clientCertificate) {
    const evalCmd = String(function doAuthX509(db, authenticatedUsername) {
        const external = db.getSiblingDB('$external');
        assert.commandWorked(external.runCommand({authenticate: 1, mechanism: 'MONGODB-X509'}));
        const connStatus = assert.commandWorked(external.adminCommand({connectionStatus: 1}));
        assert.eq(connStatus.authInfo.authenticatedUsers[0].user, authenticatedUsername);
    });

    const shell = runMongoProgram('mongo',
                                  '--host',
                                  'localhost',
                                  '--port',
                                  port,
                                  '--tls',
                                  '--tlsCAFile',
                                  serverCAFile,
                                  '--tlsCertificateKeyFile',
                                  clientCertificate,
                                  '--eval',
                                  evalCmd + ` doAuthX509(db, '${expectedUsername}');`);
    assert.eq(shell, 0);
}

function runValidMongodTest(opts, allAttrsMatch, wrongAttrValue, missingAttr) {
    const conn = MongoRunner.runMongod(opts);
    const admin = conn.getDB('admin');
    const external = conn.getDB('$external');
    assert.commandWorked(admin.runCommand({createUser: 'admin', pwd: 'admin', roles: ['root']}));
    assert(admin.auth('admin', 'admin'));

    // Incoming certificate containing all attributes in tlsClusterAuthX509Attributes should result
    // in successful auth as __system.
    authX509(allAttrsMatch.user, conn.port, allAttrsMatch.certificate);

    // Incoming certificate containing all attributes in tlsClusterAuthX509Attributes but wrong
    // value(s) should fail to auth as __system. After the subject of the cert exists as a user on
    // $external, it will succeed as that user.
    assert.throws(() => authX509('__system', conn.port, wrongAttrValue.certificate));
    assert.commandWorked(external.runCommand({createUser: wrongAttrValue.user, roles: []}));
    authX509(wrongAttrValue.user, conn.port, wrongAttrValue.certificate);

    // Incoming certificate missing some attributes in tlsClusterAuthX509Attributes
    // should fail to auth. After the subject of the cert exists as a user on $external, it will
    // succeed as that user
    assert.throws(() => authX509('__system', conn.port, missingAttr.certificate));
    assert.commandWorked(external.runCommand({createUser: missingAttr.user, roles: []}));
    authX509(missingAttr.user, conn.port, missingAttr.certificate);

    MongoRunner.stopMongod(conn);
}

checkInvalidConfigurations();

// First, run the tests with a valid set of member certificates that include one of
// DC, O, and OU but don't rely on them for membership detection.
let opts = {
    auth: '',
    tlsClusterAuthX509Attributes: clusterMembershipAttributesDN,
    clusterAuthMode: 'x509',
    tlsMode: 'preferTLS',
    tlsCertificateKeyFile: serverTitleFooCert,
    tlsCAFile: serverCAFile,
    tlsClusterFile: clusterTitleFooCert,
};
runValidMongodTest(
    opts,
    {user: '__system', certificate: clusterTitleFooCert},
    {
        user: 'title=bar,CN=server,OU=Kernel,O=MongoDB,L=New York City,ST=New York,C=US',
        certificate: serverTitleBarCert
    },
    {
        user: 'CN=server,OU=Kernel,O=MongoDB,L=New York City,ST=New York,C=US',
        certificate: serverDefaultOnlyCert
    });

// Now, use member certificates that don't have DC, O, or OU at all. This is
// valid if tlsClusterAuthX509Attributes is configured appropriately to specify
// attributes and values that the certificates have.
opts.tlsCertificateKeyFile = serverTitleFooNoDefaultCert;
opts.tlsClusterFile = clusterTitleFooNoDefaultCert;
runValidMongodTest(
    opts,
    {user: '__system', certificate: clusterTitleFooNoDefaultCert},
    {
        user: 'title=bar,CN=server,OU=Kernel,O=MongoDB,L=New York City,ST=New York,C=US',
        certificate: serverTitleBarCert
    },
    {
        user: 'CN=server,OU=Kernel,O=MongoDB,L=New York City,ST=New York,C=US',
        certificate: serverDefaultOnlyCert
    });
})();
