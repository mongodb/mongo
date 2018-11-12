// Helper script used to validate login as x509 auth with a certificate with roles works.
(function() {
    "use strict";

    // Auth as user in certificate
    let ret = db.getSiblingDB("$external").auth({
        mechanism: "MONGODB-X509",
        user:
            "CN=Kernel Client Peer Role,OU=Kernel Users,O=MongoDB,L=New York City,ST=New York,C=US"
    });
    assert.eq(ret, 1, "Auth failed");

    // Validate active roles
    let connStatus = db.runCommand('connectionStatus');
    assert.commandWorked(connStatus);

    let expectedRoles =
        [{"role": "backup", "db": "admin"}, {"role": "readAnyDatabase", "db": "admin"}];
    assert.sameMembers(connStatus.authInfo.authenticatedUserRoles, expectedRoles);
}());
