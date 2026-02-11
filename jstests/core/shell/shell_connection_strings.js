// Test mongo shell connect strings.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: eval.
//   not_allowed_with_signed_security_token,
//   uses_multiple_connections,
//   docker_incompatible,
// ]

assert(db.getMongo().uri, "Mongo object should have 'uri' property");

const mongod = new MongoURI(db.getMongo().uri).servers[0];
const host = mongod.host;
const port = mongod.port;

function testConnect(ok, ...args) {
    const exitCode = runMongoProgram("mongo", "--eval", ";", ...args);
    if (ok) {
        assert.eq(exitCode, 0, "failed to connect with `" + args.join(" ") + "`");
    } else {
        assert.neq(exitCode, 0, "unexpectedly succeeded connecting with `" + args.join(" ") + "`");
    }
}

testConnect(true, `${host}:${port}`);
testConnect(true, `${host}:${port}/test`);
testConnect(true, `${host}:${port}/admin`);
testConnect(true, host, "--port", port);
testConnect(true, "--host", host, "--port", port, "test");
testConnect(true, "--host", host, "--port", port, "admin");
testConnect(true, `mongodb://${host}:${port}/test`);
testConnect(true, `mongodb://${host}:${port}/test?connectTimeoutMS=10000`);

// if a full URI is provided, you cannot also specify host or port
testConnect(false, `${host}/test`, "--port", port);
testConnect(false, `mongodb://${host}:${port}/test`, "--port", port);
testConnect(false, `mongodb://${host}:${port}/test`, "--host", host);
testConnect(false, `mongodb://${host}:${port}/test`, "--host", host, "--port", port);

// Test that the 'uri' property returns a valid mongodb:// URI that can be used for new connections
{
    const mongo = db.getMongo();

    // The 'uri' should be usable to create a new connection
    const newMongo = new Mongo(mongo.uri);
    assert(newMongo, "Should be able to create new Mongo connection using uri property");

    const newMongo2 = connect(mongo.uri).getMongo();
    assert(newMongo2, "Should be able to create new Mongo connection using uri property");
}
