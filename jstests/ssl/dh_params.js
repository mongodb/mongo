import {dhparamSSL, replShouldSucceed, requireSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";
requireSSLProvider('openssl', function() {
    // Verify that requireTLS with Diffie-Hellman parameters allows ssl connections
    print("=== Testing that DHParams files can be loaded ===");
    replShouldSucceed("dhparam-dhparam", dhparamSSL, dhparamSSL);
});
