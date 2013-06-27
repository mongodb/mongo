// SERVER-9137 test that the httpinterface parameter enables the web interface

port = allocatePorts( 1 )[ 0 ];
var conn = MongoRunner.runMongod({ port:port, smallfiles: ""});
assert.throws( function(){
               new Mongo('localhost:'+(conn.port+1000))} , 
               null , 
               "the web interface should not be running on " + (conn.port + 1000) )
MongoRunner.stopMongod(conn);
   
conn = MongoRunner.runMongod({ port:port, smallfiles: "", httpinterface: ""});
var mongo = new Mongo('localhost:'+(conn.port+1000)) 
assert.neq(null,
           mongo, 
           "the web interface should be running on " + (conn.port + 1000));
MongoRunner.stopMongod(conn);
