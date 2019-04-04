// Helper script used to validate login as x509 auth with a certificate with roles works.
(function() {
    "use strict";

    // Auth as user in certificate with an email address
    const ret = db.getSiblingDB("$external").auth({
        mechanism: "MERIZODB-X509",
        user:
            "emailAddress=example@merizodb.com,CN=client,OU=KernelUser,O=MerizoDB,L=New York City,ST=New York,C=US"
    });
    assert.eq(ret, 1, "Auth failed");
}());
