// add a node from a different set to the current set
// I don't know what should happen here.

doTest = function( signal ) {

    var orig = new ReplSetTest( {name: 'testSet', nodes: 3} );
    orig.startSet();

    var interloper = new ReplSetTest( {name: 'testSet', nodes: 3, startPort : 31003} );
    interloper.startSet();

    sleep(5000);

    orig.initiate();
    interloper.initiate();

    sleep(5000);

    var master = orig.getMaster();

    var conf = master.getDB("local").system.replset.findOne();
 
    var nodes = interloper.nodeList();
    var host = nodes[0];
    var id = conf.members.length;
    conf.members.push({_id : id, host : host});
    conf.version++;

    try {
      var result = master.getDB("admin").runCommand({replSetReconfig : conf});
    }
    catch(e) {
      print(e);
    }
    
    // now... stuff should blow up?

    sleep(10);
}

doTest(15);