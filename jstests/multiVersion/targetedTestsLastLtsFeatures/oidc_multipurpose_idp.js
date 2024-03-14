/**
 * Test downgrade/upgrade of server with respect to OIDC multipurpose IDP support
 */
import "jstests/multiVersion/libs/multi_rs.js";

import {determineSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";
import {
    OIDCKeyServer,
    tryTokenAuth
} from "src/mongo/db/modules/enterprise/jstests/external_auth/lib/oidc_utils.js";
import {OIDCVars} from "src/mongo/db/modules/enterprise/jstests/external_auth/lib/oidc_vars.js";

if (determineSSLProvider() !== 'openssl') {
    print('Skipping test, OIDC is only available with OpenSSL');
    quit();
}

const keyMap = {
    issuer1: 'src/mongo/db/modules/enterprise/jstests/external_auth/lib/custom-key-1.json',
    issuer2: 'src/mongo/db/modules/enterprise/jstests/external_auth/lib/custom-key-1.json'
};
const KeyServer = new OIDCKeyServer(JSON.stringify(keyMap));
const issuer1 = KeyServer.getURL() + '/issuer1';
const issuer2 = KeyServer.getURL() + '/issuer2';
const kOIDCConfig = [
    {
        issuer: issuer1,
        audience: 'jwt@kernel.mongodb.com',
        authNamePrefix: 'issuer1',
        matchPattern: '@mongodb.com$',
        clientId: 'deadbeefcafe',
        requestScopes: ['email'],
        principalName: 'sub',
        authorizationClaim: 'mongodb-roles',
        logClaims: ['sub', 'aud', 'mongodb-roles', 'does-not-exist'],
        JWKSPollSecs: 15,
    },
    {
        issuer: issuer2,
        audience: 'jwt@kernel.mongodb.com',
        authNamePrefix: 'issuer2',
        matchPattern: '@10gen.com$',
        clientId: 'deadbeefcafe',
        authorizationClaim: 'mongodb-roles',
        JWKSPollSecs: 15,
    }
];

const startupOptions = {
    authenticationMechanisms: 'SCRAM-SHA-256,MONGODB-OIDC',
    oidcIdentityProviders: tojson(kOIDCConfig),
};

const startupOptionsNotDowngradable = {
    authenticationMechanisms: 'SCRAM-SHA-256,MONGODB-OIDC',
    oidcIdentityProviders: tojson(kOIDCConfig.concat({
        issuer: issuer1,
        audience: 'jwt@kernel.10gen.com',
        authNamePrefix: 'issuer1-alt',
        authorizationClaim: 'mongodb-roles',
        supportsHumanFlows: false,
        JWKSPollSecs: 15,
    })),
};

const {
    'Token_OIDCAuth_user1': issuerOneToken,
    'Token_OIDCAuth_user1@10gen': issuerTwoToken,
    'Token_OIDCAuth_user1_alt_audience': issuerOneAltAudienceToken,
} = OIDCVars(KeyServer.getURL()).kOIDCTokens;

function setup(conn) {
    const adminDB = conn.getDB('admin');
    assert.commandWorked(conn.adminCommand({createUser: 'admin', 'pwd': 'foo', roles: ['root']}));

    // Create the roles corresponding to user1@mongodb.com and user1@10gen.com's groups.
    assert.commandWorked(conn.adminCommand(
        {createRole: 'issuer1/myReadRole', roles: ['readAnyDatabase'], privileges: []}));
    assert.commandWorked(
        conn.adminCommand({createRole: 'issuer2/myReadRole', roles: ['read'], privileges: []}));
    assert.commandWorked(
        conn.adminCommand({createRole: 'issuer1-alt/myreadRole', roles: ['read'], privileges: []}));
    // Increase logging verbosity.
    assert.commandWorked(adminDB.setLogLevel(3));
}

function runAuthTest(hostname, altCanAuth) {
    let conn = new Mongo(hostname);
    assert(tryTokenAuth(conn, issuerOneToken));
    assert.commandWorked(conn.adminCommand({listDatabases: 1}));
    conn.close();

    conn = new Mongo(hostname);
    assert(tryTokenAuth(conn, issuerTwoToken));
    assert.commandWorked(conn.adminCommand({listDatabases: 1}));
    conn.close();

    conn = new Mongo(hostname);
    assert(tryTokenAuth(conn, issuerOneAltAudienceToken) == altCanAuth);
    if (altCanAuth) {
        assert.commandWorked(conn.adminCommand({listDatabases: 1}));
    }
    conn.close();
}

function testBinaryUpgrade(initConfig, altCanAuthBefore, newConfig, altCanAuthAfter) {
    jsTestLog("Testing upgrade from last-lts to latest");
    const rst = new ReplSetTest(
        {nodes: 2, nodeOptions: {binVersion: 'last-lts', setParameter: initConfig}});
    rst.startSet();
    rst.initiate();

    let conn = rst.getPrimary();
    setup(conn);
    runAuthTest(conn.host, altCanAuthBefore);

    jsTestLog("Starting binary upgrade to latest");
    const cfgWithFeatureFlag = Object.assign({featureFlagOIDCMultipurposeIDP: true}, newConfig)
    rst.upgradeSet({binVersion: 'latest', setParameter: cfgWithFeatureFlag});
    jsTestLog("Finished binary upgrade to latest");

    conn = rst.getPrimary();
    runAuthTest(conn.host, altCanAuthAfter);

    rst.stopSet();
}

function testBinaryDowngrade(initConfig, altCanAuthBefore, newConfig, altCanAuthAfter) {
    jsTestLog("Testing downgrade from latest to last-lts");
    const cfgWithFeatureFlag = Object.assign({featureFlagOIDCMultipurposeIDP: true}, initConfig)
    const rst = new ReplSetTest({nodes: 2, nodeOptions: {setParameter: cfgWithFeatureFlag}});
    rst.startSet();
    rst.initiate();

    let conn = rst.getPrimary();
    setup(conn);
    runAuthTest(conn.host, altCanAuthBefore);

    {
        const adminDB = conn.getDB('admin');
        assert.commandWorked(adminDB.runCommand(
            {setFeatureCompatibilityVersion: lastLTSFCV, confirm: true, writeConcern: {w: 1}}));
        checkFCV(adminDB, lastLTSFCV);
        adminDB.logout();
    }

    jsTestLog("Starting binary downgrade to last LTS");
    rst.upgradeSet({binVersion: 'last-lts', setParameter: newConfig});
    jsTestLog("Finished binary downgrade to last LTS");

    conn = rst.getPrimary();
    runAuthTest(conn.host, altCanAuthAfter);

    rst.stopSet();
}

KeyServer.start();

testBinaryUpgrade(startupOptions, false, startupOptions, false);
testBinaryUpgrade(startupOptions, false, startupOptionsNotDowngradable, true);

testBinaryDowngrade(startupOptionsNotDowngradable, true, startupOptions, false);
testBinaryDowngrade(startupOptions, false, startupOptions, false);

KeyServer.stop();
