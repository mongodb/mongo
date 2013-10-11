// If we are running in use-x509 passthrough mode, turn it off or else the auth 
// part of this test will not work correctly

TestData.useX509 = false;
var SERVER_CERT = "jstests/libs/server.pem"
var CA_CERT = "jstests/libs/ca.pem" 

var CLIENT_USER = "CN=client,OU=kerneluser,O=10Gen,L=New York City,ST=New York,C=US"
var INVALID_CLIENT_USER = "CN=invalidclient,OU=kerneluser,O=10Gen,L=New York City,ST=New York,C=US"

port = allocatePorts(1)[0];

function authAndTest(mongo) {
    external = mongo.getDB("$external")
    test = mongo.getDB("test");

    // Add user using localhost exception
    external.addUser({user: CLIENT_USER, roles:[
            {'role':'userAdminAnyDatabase', 'db':'admin'}, 
            {'role':'readWriteAnyDatabase', 'db':'admin'}]})

    // Localhost exception should not be in place anymore
    assert.throws( function() { test.foo.findOne()}, {}, "read without login" )

    assert( !external.auth({user: INVALID_CLIENT_USER, mechanism: 'MONGODB-X509'}),
            "authentication with invalid user failed" )
    assert( external.auth({user: CLIENT_USER, mechanism: 'MONGODB-X509'}),
            "authentication with valid user failed" )

    // Check that we can add a user and read data
    test.addUser({user: "test", pwd: "test", roles:[ 
            {'role': 'readWriteAnyDatabase', 'db': 'admin'}]})
    test.foo.findOne()

    external.logout();
    assert.throws( function() { test.foo.findOne()}, {}, "read after logout" )
}

print("1. Testing x.509 auth to mongod");
var mongo = MongoRunner.runMongod({port : port,
                                sslMode : "sslOnly", 
                                sslPEMKeyFile : SERVER_CERT, 
                                sslCAFile : CA_CERT,
                                auth:""});

authAndTest(mongo);
stopMongod(port);

print("2. Testing x.509 auth to mongos");
var x509_options = {sslMode : "sslOnly",
                    sslPEMKeyFile : SERVER_CERT,
                    sslCAFile : CA_CERT};

var st = new ShardingTest({ shards : 1,
                            mongos : 1,
                            other: {
                                extraOptions : {"keyFile" : "jstests/libs/key1"},
                                configOptions : x509_options,
                                mongosOptions : x509_options,
                            }});

authAndTest(new Mongo("localhost:" + st.s0.port))
