if ( ! _isWindows() ) {
    hoststring = db.getMongo().host
    index = hoststring.lastIndexOf(':')
    if (index == -1){
        port = '27017'
    } else {
        port = hoststring.substr(index + 1)
    }

    sock = new Mongo('/tmp/mongodb-' + port + '.sock')
    sockdb = sock.getDB(db.getName())
    assert( sockdb.runCommand('ping').ok );

    // test unix socket path
    var ports = allocatePorts(1);
    var path = "/data/db/sockpath";
    
    var conn = new MongodRunner(ports[0], path, null, null, ["--unixSocketPrefix", path]);
    conn.start();
    
    var sock2 = new Mongo(path+"/mongodb-"+ports[0]+".sock");
    sockdb2 = sock2.getDB(db.getName())
    assert( sockdb2.runCommand('ping').ok );
} else {
    print("Not testing unix sockets on Windows");
}
