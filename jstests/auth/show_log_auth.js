// test that "show log dbname" and "show logs" have good err messages when unauthorized

var port = allocatePorts( 1 )[ 0 ];
var baseName = "jstests_show_log_auth";

var m = startMongod( "--auth", "--port", port, "--dbpath", "/data/db/" + baseName, "--nohttpinterface", "--bind_ip", "127.0.0.1" , "--nojournal" , "--smallfiles" );
var db = m.getDB( "admin" );

db.addUser( "admin" , "pass", jsTest.adminUserRoles );

// Temporarily capture this shell's print() output
var oldprint = print, printed = [];
print = function(s) { printed.push(s); }

try {
    shellHelper.show('logs');
    shellHelper.show('log ' + baseName);
}
finally {
    // Stop capturing print() output
    print = oldprint;
}

function assertStartsWith(s, prefix) {
    assert.eq(s.substr(0, prefix.length), prefix);
}

assertStartsWith(printed[0], 'Error while trying to show logs');
assertStartsWith(printed[1], 'Error while trying to show ' + baseName + ' log');

db.auth( "admin" , "pass" );
db.shutdownServer();
