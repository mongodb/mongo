// Test mixed sslMode disabled/allowSSL, this test cannot be run
// from the /ssl directory since the --use-ssl passthrough
// will make it impossible for the shell to connect to the replicas
ssl_options1 = {sslMode : "disabled"};
ssl_options2 = {sslMode : "allowSSL",
               sslPEMKeyFile : "jstests/libs/server.pem",
               sslCAFile: "jstests/libs/ca.pem"};
load("jstests/replsets/replset1.js");
