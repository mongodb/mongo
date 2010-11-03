/*
 * 1. insert 100000 objects
 * 2. export to two dbpaths
 * 3. add one node w/fastsync
 * 4. check that we never get "errmsg" : "initial sync cloning db: whatever"
 * 5. check writes are replicated
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

ports = allocatePorts( 3 );

var basename = "jstests_fastsync";
var basePath = "/data/db/" + basename;
var hostname = getHostName();

var pargs = new MongodRunner( ports[ 0 ], basePath + "-p", false, false,
                              ["--replSet", basename], {no_bind : true} );
p = pargs.start();

var admin = p.getDB("admin");
var foo = p.getDB("foo");
var local = p.getDB("local");

var config = {_id : basename, members : [{_id : 0, host : hostname+":"+ports[0]}]};
printjson(config);
var result = admin.runCommand({replSetInitiate : config});
assert(result.ok, "initiate failed");
assert.soon(function() { return admin.runCommand({isMaster:1}).ismaster; });

print("1");
for (var i=0; i<100000; i++) {
  foo.bar.insert({date : new Date(), x : i, str : "all the talk on the market"});
}
print("total in foo: "+foo.bar.count());


print("2");
admin.runCommand( {fsync:1,lock:1} );

copyDbpath( basePath + "-p", basePath + "-s" );

print("remove local files or slave will get confused");
var files = listFiles(basePath+"-s");

for (var i in files) {
  var filename = files[i].name;
  if (filename.match("/local")) {
    print("removing "+filename);
    removeFile(filename);
  }
}
//run("rm", basePath+"-s/local.*");
    
admin.$cmd.sys.unlock.findOne();


print("3");
var sargs = new MongodRunner( ports[ 1 ], basePath + "-s", false, false,
                              ["--replSet", basename, "--fastsync"],
                              {no_bind : true} );
var reuseData = true;
sargs.start(reuseData);

config = local.system.replset.findOne();
config.version++;
config.members.push({_id:1, host:hostname+":"+ports[1]});

result = admin.runCommand({replSetReconfig : config});
assert(result.ok, "reconfig worked");
reconnect(p);

print("4");
var status = admin.runCommand({replSetGetStatus : 1});
var count = 0;
while (status.members[1].state != 2 && count < 200) {
  print("not a secondary yet");
  if (count % 10 == 0) {
    printjson(status);
  }
  assert(!status.members[1].errmsg || !status.members[1].errmsg.match("^initial sync cloning db"));
  
  sleep(1000);

  // disconnection could happen here
  try {  
    status = admin.runCommand({replSetGetStatus : 1});
  }
  catch (e) {
    print(e);
  }
  count++;
}

assert.eq(status.members[1].state, 2);
