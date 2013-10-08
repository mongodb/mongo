ssl_options = {sslMode : "sslOnly",
               sslPEMKeyFile : "jstests/libs/server.pem",
               sslCAFile: "jstests/libs/ca.pem"};

load("jstests/replsets/replset1.js");
