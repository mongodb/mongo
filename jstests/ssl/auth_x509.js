// Basic tests for cluster authentication using x509
// This test is launching replsets/initial_sync1.js with different 
// values for clusterAuthMode to emulate an upgrade process.

var common_options = {sslOnNormalPorts : "",
               sslPEMKeyFile : "jstests/libs/server.pem",
               sslCAFile: "jstests/libs/ca.pem",
               keyFile : "jstests/libs/key1"};

// Standard case, clusterAuthMode: x509
x509_options1 = Object.merge(common_options, 
                             {sslClusterFile: "jstests/libs/cluster-cert.pem",
                             clusterAuthMode: "x509"});
var x509_options2 = x509_options1;
load("jstests/replsets/initial_sync_1.js");

// Mixed clusterAuthMode: sendX509 and sendKeyfile and try adding --auth 
x509_options1 = Object.merge(common_options, 
                             {sslClusterFile: "jstests/libs/cluster-cert.pem",
                             clusterAuthMode: "x509",
                             auth: ""});
x509_options2 = Object.merge(common_options, {clusterAuthMode: "sendKeyfile"});
load("jstests/replsets/initial_sync1.js");

// Mixed clusterAuthMode: x509 and sendX509, use the PEMKeyFile for outgoing connections 
x509_options1 = Object.merge(common_options, {clusterAuthMode: "x509"});
x509_options2 = Object.merge(common_options, {clusterAuthMode: "sendX509"});
load("jstests/replsets/initial_sync1.js");

//verify that replset initiate fails if using an invalid cert
x509_options1 = Object.merge(common_options, {clusterAuthMode: "x509"});
x509_options2 = Object.merge(common_options, 
                             {sslClusterFile: "jstests/libs/smoke.pem",
                             clusterAuthMode: "x509"});
var replTest = new ReplSetTest({nodes : {node0 : x509_options1, node1 : x509_options2}});
var conns = replTest.startSet();
assert.throws( function() { replTest.initiate() } );
