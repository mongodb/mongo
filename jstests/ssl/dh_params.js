load("jstests/ssl/libs/ssl_helpers.js");
requireSSLProvider('openssl', function() {
    "use strict";

    // Verify that requireSSL with Diffie-Hellman parameters allows ssl connections
    print("=== Testing that DHParams files can be loaded ===");
    replShouldSucceed("dhparam-dhparam", dhparamSSL, dhparamSSL);
});
