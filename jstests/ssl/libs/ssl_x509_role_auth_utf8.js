// Helper script used to validate login as x509 auth with a certificate with roles works.
(function() {
"use strict";

// Authenticate against a certificate with a RDN in the subject name of type UTF8STRING
const retutf8 = db.getSiblingDB("$external").auth({
    mechanism: "MONGODB-X509",
    user:
        "C=US,ST=New York,L=New York City,O=MongoDB,OU=Kernel Users,CN=\\D0\\9A\\D0\\B0\\D0\\BB\\D0\\BE\\D1\\8F\\D0\\BD"
});
assert.eq(retutf8, 1, "Auth failed");
}());
