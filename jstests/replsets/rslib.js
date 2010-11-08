
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
}

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
