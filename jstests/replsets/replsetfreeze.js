/*
 * 1: initialize set
 * 2: step down m1
 * 3: freeze set for 30 seconds
 * 4: check no one is master for 30 seconds
 * 5: check for new master
 * 6: step down new master
 * 7: freeze for 30 seconds
 * 8: unfreeze
 * 9: check we get a new master within 30 seconds
 */


var w = 0;
var wait = function(f) {
    w++;
    var n = 0;
    while (!f()) {
        if( n % 4 == 0 )
            print("toostale.js waiting " + w);
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
        a.getDB("foo").bar.stats();
        return true;
      } catch(e) {
        print(e);
        return false;
      }
    });
};


print("1: initialize set");
var replTest = new ReplSetTest( {name: 'unicomplex', nodes: 3} );
var nodes = replTest.nodeList();
var conns = replTest.startSet();
var config = {"_id" : "unicomplex", "members" : [
    {"_id" : 0, "host" : nodes[0] },
    {"_id" : 1, "host" : nodes[1] },
    {"_id" : 2, "host" : nodes[2], "arbiterOnly" : true}]};
var r = replTest.initiate(config);
var master = replTest.getMaster();


print("2: step down m1");
try {
  master.getDB("admin").runCommand({replSetStepDown : 1, force : 1});
}
catch(e) {
  print(e);
}
reconnect(master);

print("3: freeze set for 30 seconds");
master.getDB("admin").runCommand({replSetFreeze : 30});


print("4: check no one is master for 30 seconds");
var start = (new Date()).getTime();
while ((new Date()).getTime() - start < 30000) {
  var result = master.getDB("admin").runCommand({isMaster:1});
  assert.eq(result.ismaster, false);
  assert.eq(result.primary, undefined);
  sleep(1000);
}


print("5: check for new master");
master = replTest.getMaster();


print("6: step down new master");
try {
  master.getDB("admin").runCommand({replSetStepDown : 1, force : 1});
}
catch(e) {
  print(e);
}
reconnect(master);


print("7: freeze for 30 seconds");
master.getDB("admin").runCommand({replSetFreeze : 30});
sleep(1000);


print("8: unfreeze");
master.getDB("admin").runCommand({replSetFreeze : 0});


print("9: check we get a new master within 30 seconds");
master = replTest.getMaster();


replTest.stopSet( 15 );

