// Helper script used to validate login as x509 auth with a certificate with roles works.
(function() {
"use strict";

// Auth as user in certificate with an email address
const ret = db.getSiblingDB("$external").auth({
    mechanism: "MONGODB-X509",
    user:
        "emailAddress=example@mongodb.com,CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US"
});
assert.eq(ret, 1, "Auth failed");
}());
