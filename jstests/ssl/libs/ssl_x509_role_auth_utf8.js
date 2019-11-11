// Helper script used to validate login as x509 auth with a certificate with roles works.
(function() {
"use strict";

// Authenticate against a certificate with a RDN in the subject name of type UTF8STRING
const retutf8 = db.getSiblingDB("$external").auth({
    mechanism: "MONGODB-X509",
    user:
        "CN=\\D0\\9A\\D0\\B0\\D0\\BB\\D0\\BE\\D1\\8F\\D0\\BD,OU=Kernel Users,O=MongoDB,L=New York City,ST=New York,C=US"
});
assert.eq(retutf8, 1, "Auth failed");
}());
