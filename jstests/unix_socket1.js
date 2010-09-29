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
    assert( sockdb.runCommand('ping').ok )

} else {
    print("Not testing unix sockets on Windows");
}
