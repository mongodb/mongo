// Helper script used to validate login as x509 auth with a certificate with roles works.
(function() {
"use strict";

// Auth as user in certificate with a subject name with lots of RFC 2253 escaping
// Ex: CN=Test,OU=Escape,O=\;\ ,L=\ \>,ST=\"\\\<,C=\,\+
// It validates leading space, and the 7 magic characters
const ret = db.getSiblingDB("$external").auth({
    mechanism: "MONGODB-X509",
    user: "CN=Test,OU=Escape,O=\\;\\ ,L=\\ \\>,ST=\\\"\\\\\\<,C=\\,\\+"
});
assert.eq(ret, 1, "Auth failed");
}());
