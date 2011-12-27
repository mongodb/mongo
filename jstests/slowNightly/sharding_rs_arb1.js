x = 5
name = "sharding_rs_arb1"
replTest = new ReplSetTest( { name : name , nodes : 3 , startPort : 31000 } );
nodes = replTest.startSet();
var port = replTest.ports;
replTest.initiate({_id : name, members :
        [
            {_id:0, host : getHostName()+":"+port[0]},
            {_id:1, host : getHostName()+":"+port[1]},
            {_id:2, host : getHostName()+":"+port[2], arbiterOnly : true},
        ],
                  });

replTest.awaitReplication();

master = replTest.getMaster();
db = master.getDB( "test" );
printjson( rs.status() );

var config = startMongodEmpty("--configsvr", "--port", 29999, "--dbpath", "/data/db/" + name + "_config" );

var mongos = startMongos({ port : 30000, configdb : getHostName() + ":29999" })
var admin = mongos.getDB("admin")
var url = name + "/";
for ( i=0; i<port.length; i++ ) {
    if ( i > 0 )
        url += ",";
    url += getHostName() + ":" + port[i];
}
print( url )
res = admin.runCommand( { addshard : url } )
printjson( res )
assert( res.ok , tojson(res) )



stopMongod( 30000 )
stopMongod( 29999 )
replTest.stopSet();

