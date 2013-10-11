// Test mixed sslMode noSSL/acceptSSL, this test cannot be run
// from the /ssl directory since the --use-ssl passthrough
// will make it impossible for the shell to connect to the replicas
ssl_options1 = {sslMode : "noSSL"};
ssl_options2 = {sslMode : "acceptSSL",
               sslPEMKeyFile : "jstests/libs/server.pem",
               sslCAFile: "jstests/libs/ca.pem"};
load("jstests/replsets/replset1.js");
