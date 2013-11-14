// If we are running in use-x509 passthrough mode, turn it off
// since it is not necessary for this test.
TestData.useX509 = false;

ssl_options1 = {sslMode : "requireSSL",
               sslPEMKeyFile : "jstests/libs/server.pem",
               sslCAFile: "jstests/libs/ca.pem",
               sslAllowInvalidCertificates: ""};
ssl_options2 = ssl_options1;
load("jstests/replsets/replset1.js");

// Test mixed sslMode allowSSL/preferSSL
ssl_options1 = {sslMode : "allowSSL",
               sslPEMKeyFile : "jstests/libs/server.pem",
               sslCAFile: "jstests/libs/ca.pem",
               sslAllowInvalidCertificates: ""};
ssl_options2 = {sslMode : "preferSSL",
               sslPEMKeyFile : "jstests/libs/server.pem",
               sslCAFile: "jstests/libs/ca.pem",
               sslAllowInvalidCertificates: ""};
load("jstests/replsets/replset1.js");

// Test mixed sslMode preferSSL/requireSSL
ssl_options1 = {sslMode : "preferSSL",
               sslPEMKeyFile : "jstests/libs/server.pem",
               sslCAFile: "jstests/libs/ca.pem",
               sslAllowInvalidCertificates: ""};
ssl_options2 = {sslMode : "requireSSL",
               sslPEMKeyFile : "jstests/libs/server.pem",
               sslCAFile: "jstests/libs/ca.pem",
               sslAllowInvalidCertificates: ""};
load("jstests/replsets/replset1.js");
