
var count = 0;
var w = 0;

var wait = function(f) {
    w++;
    var n = 0;
    while (!f()) {
        if( n % 4 == 0 )
            print("waiting " + w);
        if (++n == 4) {
            print("" + f);
        }
        assert(n < 200, 'tried 200 times, giving up');
        sleep(1000);
    }
};

/**
 * Use this to do something once every 4 iterations.
 *
 * <pre>
 * for (i=0; i<1000; i++) {
 *   occasionally(function() { print("4 more iterations"); });
 * }
 * </pre>
 */
var occasionally = function(f, n) {
  var interval = n || 4;
  if (count % interval == 0) {
    f();
  }
  count++;
};
  
var reconnect = function(a) {
  wait(function() { 
      try {
        // make this work with either dbs or connections
        if (typeof(a.getDB) == "function") {
          a.getDB("foo").bar.stats();
        }
        else {
          a.bar.stats();
        }
        return true;
      } catch(e) {
        print(e);
        return false;
      }
    });
};


var getLatestOp = function(server) {
    server.getDB("admin").getMongo().setSlaveOk();
    var log = server.getDB("local")['oplog.rs'];
    var cursor = log.find({}).sort({'$natural': -1}).limit(1);
    if (cursor.hasNext()) {
      return cursor.next();
    }
    return null;
};
