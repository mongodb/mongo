doesLogMatchRegex = function(logArray, regex) {
    for (var i = (logArray.length - 1); i >= 0; i--) {
        var regexInLine = regex.exec(logArray[i]);
        if (regexInLine != null) {
            return true;
        }
    }
    return false;
};

if (!_isWindows()) {
    hoststring = db.getMongo().host;
    index = hoststring.lastIndexOf(':');
    if (index == -1) {
        port = '27017';
    } else {
        port = hoststring.substr(index + 1);
    }

    sock = new Mongo('/tmp/mongodb-' + port + '.sock');
    sockdb = sock.getDB(db.getName());
    assert(sockdb.runCommand('ping').ok);

    // Test unix socket path
    var path = MongoRunner.dataDir + "/sockpath";
    mkdir(path);
    var dataPath = MongoRunner.dataDir + "/sockpath_data";

    var conn = MongoRunner.runMongod({dbpath: dataPath, unixSocketPrefix: path});

    var sock2 = new Mongo(path + "/mongodb-" + conn.port + ".sock");
    sockdb2 = sock2.getDB(db.getName());
    assert(sockdb2.runCommand('ping').ok);

    // Test the naming of the unix socket
    var log = db.adminCommand({getLog: 'global'});
    var ll = log.log;
    var re = new RegExp("anonymous unix socket");
    assert(doesLogMatchRegex(ll, re), "Log message did not contain 'anonymous unix socket'");
} else {
    print("Not testing unix sockets on Windows");
}
