// Test restricting role authorization via X509 extensions.
load('jstests/ssl/libs/ssl_helpers.js');

requireSSLProvider('openssl', function() {
    "use strict";

    const SERVER_CERT = 'jstests/libs/server.pem';
    const COMBINED_CA_CERT = 'jstests/ssl/x509/root-and-trusted-ca.pem';
    const CA_HASH = '539D91F8202641BF85C0C36C88FF69F3062D4AB370CECBF9B950A8B97DE72EAE';
    const TRUSTED_CA_HASH = 'AEAEBB1BA947A7C1428D39EF6166B83409D0245D28013C9FDD71DF9E69BEA52B';

    // Common suffix, keep the lines short.
    const RDN_SUFFIX = ',O=MongoDB,L=New York City,ST=New York,C=US';
    const USERS = [];

    const CLIENT = {
        cert: 'jstests/libs/client.pem',
        roles: [],
    };
    USERS.push('CN=client,OU=KernelUser');

    const CLIENT_ROLES = {
        cert: 'jstests/libs/client_roles.pem',
        roles: [{role: 'backup', db: 'admin'}, {role: 'readAnyDatabase', db: 'admin'}],
    };
    USERS.push('CN=Kernel Client Peer Role,OU=Kernel Users');

    const TRUSTED_CLIENT_TESTDB_ROLES = {
        cert: 'jstests/ssl/x509/trusted-client-testdb-roles.pem',
        roles: [{role: 'role1', db: 'testDB'}, {role: 'role2', db: 'testDB'}],
    };
    USERS.push('CN=Trusted Kernel Test Client With Roles,OU=Kernel Users');

    function test(tlsCATrusts, success, failure) {
        const options = {
            auth: '',
            tlsMode: 'requireTLS',
            tlsCertificateKeyFile: SERVER_CERT,
            tlsCAFile: COMBINED_CA_CERT,
        };

        if (tlsCATrusts !== null) {
            options.setParameter = {
                tlsCATrusts: tojson(tlsCATrusts),
            };
        }

        const mongod = MongoRunner.runMongod(options);

        const admin = mongod.getDB('admin');
        admin.createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
        admin.auth({user: 'admin', pwd: 'pwd'});

        const external = mongod.getDB('$external');
        USERS.forEach((u) => external.createUser({user: u + RDN_SUFFIX, roles: []}));

        const testDB = mongod.getDB('test');
        testDB.createRole({role: 'role1', privileges: [], roles: []});
        testDB.createRole({role: 'role2', privileges: [], roles: []});

        // Sorting JS arrays of objects with arbitrary order is... complex.
        const serverTrusts =
            assert.commandWorked(admin.runCommand({getParameter: 1, tlsCATrusts: 1})).tlsCATrusts;
        function sortAndNormalizeRoles(roles) {
            return roles.map((r) => r.role + '.' + r.db).sort().join('/');
        }
        function sortAndNormalizeTrusts(trusts) {
            if (trusts === null) {
                return "(unconfigured)";
            }
            return trusts.map((t) => t.sha256 + '/' + sortAndNormalizeRoles(t.roles)).sort();
        }
        assert.eq(sortAndNormalizeTrusts(tlsCATrusts), sortAndNormalizeTrusts(serverTrusts));

        function impl(user, expect) {
            const snRoles = tojson(sortAndNormalizeRoles(user.roles));
            const uri = 'mongodb://localhost:' + mongod.port + '/admin';
            const script = tojson(sortAndNormalizeRoles) +
                'assert(db.getSiblingDB("$external").auth({mechanism: "MONGODB-X509"}));' +
                'const status = assert.commandWorked(db.runCommand({connectionStatus: 1}));' +
                'const roles = status.authInfo.authenticatedUserRoles;' +
                'assert.eq(' + snRoles + ', sortAndNormalizeRoles(roles));';
            const mongo = runMongoProgram('mongo',
                                          '--tls',
                                          '--tlsCertificateKeyFile',
                                          user.cert,
                                          '--tlsCAFile',
                                          COMBINED_CA_CERT,
                                          uri,
                                          '--eval',
                                          script);
            expect(mongo, 0);
        }

        success.forEach((u) => impl(u, assert.eq));
        failure.forEach((u) => impl(u, assert.neq));

        MongoRunner.stopMongod(mongod);
    }

    // Positive tests.
    const unconfigured = null;
    test(unconfigured, [CLIENT, CLIENT_ROLES, TRUSTED_CLIENT_TESTDB_ROLES], []);

    const allRoles = [
        {sha256: CA_HASH, roles: [{role: '', db: ''}]},
        {sha256: TRUSTED_CA_HASH, roles: [{role: '', db: ''}]}
    ];
    test(allRoles, [CLIENT, CLIENT_ROLES, TRUSTED_CLIENT_TESTDB_ROLES], []);

    const allRolesOnAdmin = [{sha256: CA_HASH, roles: [{role: '', db: 'admin'}]}];
    test(allRolesOnAdmin, [CLIENT, CLIENT_ROLES], [TRUSTED_CLIENT_TESTDB_ROLES]);

    const specificRolesOnAnyDB =
        [{sha256: CA_HASH, roles: [{role: 'backup', db: ''}, {role: 'readAnyDatabase', db: ''}]}];
    test(specificRolesOnAnyDB, [CLIENT, CLIENT_ROLES], [TRUSTED_CLIENT_TESTDB_ROLES]);

    const exactRoles = [{
        sha256: CA_HASH,
        roles: [{role: 'backup', db: 'admin'}, {role: 'readAnyDatabase', db: 'admin'}]
    }];
    test(exactRoles, [CLIENT, CLIENT_ROLES], [TRUSTED_CLIENT_TESTDB_ROLES]);

    const extraRoles = [{
        sha256: CA_HASH,
        roles: [
            {role: 'backup', db: 'admin'},
            {role: 'readAnyDatabase', db: 'admin'},
            {role: 'readWrite', db: 'admin'}
        ]
    }];
    test(extraRoles, [CLIENT, CLIENT_ROLES], [TRUSTED_CLIENT_TESTDB_ROLES]);

    const similarRoles = [
        {
            sha256: CA_HASH,
            roles: [
                {role: 'backup', db: 'test'},
                {role: 'readAnyDatabase', db: ''},
                {role: 'backup', db: 'admin'}
            ]
        },
        {
            sha256: TRUSTED_CA_HASH,
            roles: [
                {role: 'role1', db: 'admin'},
                {role: 'role2', db: 'testDB'},
                {role: 'role1', db: 'testDB'},
            ]
        }
    ];
    test(similarRoles, [CLIENT, CLIENT_ROLES, TRUSTED_CLIENT_TESTDB_ROLES], []);

    const withUntrusted =
        [{sha256: CA_HASH, roles: [{role: '', db: ''}]}, {sha256: TRUSTED_CA_HASH, roles: []}];
    test(withUntrusted, [CLIENT, CLIENT_ROLES], [TRUSTED_CLIENT_TESTDB_ROLES]);

    const customRoles = [{
        sha256: TRUSTED_CA_HASH,
        roles: [
            {role: 'role1', db: 'testDB'},
            {role: 'role2', db: 'testDB'},
        ]
    }];
    test(customRoles, [CLIENT, TRUSTED_CLIENT_TESTDB_ROLES], [CLIENT_ROLES]);

    // Negative tests. CLIENT_CERT is okay because it doesn't ask for roles.
    const noTrustedCAs = [];
    test(noTrustedCAs, [CLIENT], [CLIENT_ROLES, TRUSTED_CLIENT_TESTDB_ROLES]);

    const noRoles = [{sha256: CA_HASH, roles: []}];
    test(noRoles, [CLIENT], [CLIENT_ROLES, TRUSTED_CLIENT_TESTDB_ROLES]);

    const insufficientRoles1 = [
        {sha256: CA_HASH, roles: [{role: 'backup', db: ''}]},
        {sha256: TRUSTED_CA_HASH, roles: [{role: 'role1', db: 'testDB'}]}
    ];
    test(insufficientRoles1, [CLIENT], [CLIENT_ROLES, TRUSTED_CLIENT_TESTDB_ROLES]);

    const insufficientRoles2 = [
        {sha256: CA_HASH, roles: [{role: 'readWriteAnyDatabase', db: ''}]},
        {sha256: TRUSTED_CA_HASH, roles: [{role: 'role2', db: 'testDB'}]}
    ];
    test(insufficientRoles2, [CLIENT], [CLIENT_ROLES, TRUSTED_CLIENT_TESTDB_ROLES]);

    const withTrusted =
        [{sha256: CA_HASH, roles: []}, {sha256: TRUSTED_CA_HASH, roles: [{role: '', db: ''}]}];
    test(withTrusted, [CLIENT, TRUSTED_CLIENT_TESTDB_ROLES], [CLIENT_ROLES]);
});
