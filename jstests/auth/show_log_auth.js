// test that "show log dbname" and "show logs" have good err messages when unauthorized

var baseName = "jstests_show_log_auth";

var m = MongoRunner.runMongod({auth: "", bind_ip: "127.0.0.1", nojournal: "", smallfiles: ""});
var db = m.getDB("admin");

db.createUser({user: "admin", pwd: "pass", roles: jsTest.adminUserRoles});

function assertStartsWith(s, prefix) {
    assert.eq(s.substr(0, prefix.length), prefix);
}

assertStartsWith(print
                     .captureAllOutput(function() {
                         shellHelper.show('logs');
                     })
                     .output[0],
                 'Error while trying to show logs');

assertStartsWith(print
                     .captureAllOutput(function() {
                         shellHelper.show('log ' + baseName);
                     })
                     .output[0],
                 'Error while trying to show ' + baseName + ' log');

db.auth("admin", "pass");
db.shutdownServer();
