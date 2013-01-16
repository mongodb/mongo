// Test connecting to an SSL-mongod with a non-SSL mongo shell
// The server should not crash, the shell should not hang, and the server should log an error.

// Fetch mongod port
var opts = db.serverCmdLineOpts();
var port = opts.parsed.port;

// Generate a failure message in the log because we are attempting to connect without --ssl
var mongo = runMongoProgram("mongo", "--port", port);

// Search log for message
var log = db.adminCommand({getLog:"global"}).log;

found = false;
for (var i=log.length - 1; i >= 0; i--) {
    if (log[i].indexOf("unknown protocol") >= 0) {
        found = true;
        break;
    }
}

assert(found);

