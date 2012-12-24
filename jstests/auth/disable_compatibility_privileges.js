// Test disabling old-style privilege documents at server startup.

function assertGLEOK(status) {
    assert(status.ok && status.err === null,
           "Expected OK status object; found " + tojson(status));
}

function assertGLENotOK(status) {
    assert(status.ok && status.err !== null,
           "Expected not-OK status object; found " + tojson(status));
}

var conn = MongoRunner.runMongod({ auth: "",
                                   smallfiles: "",
                                   setParameter: "supportCompatibilityFormPrivilegeDocuments=false"
                                 });
var test = conn.getDB("test");

// Valid compatibility document shoudl fail.
test.system.users.insert({ user: "spencer", pwd: hex_md5("spencer:mongo:a"), readOnly: true });
assertGLENotOK(test.getLastErrorObj());

test.system.users.insert({ user: "spencer", userSource: "test2", roles: ["dbAdmin"] });
assertGLEOK(test.getLastErrorObj());
