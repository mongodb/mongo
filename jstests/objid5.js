
t = db.objid5;
t.drop();

t.save( { _id : 5.5 } );
assert.eq( 18 , Object.bsonsize( t.findOne() ) , "A" );


x = db.runCommand( { features : 1 } )
y = db.runCommand( { features : 1 , oidReset : 1 } )

assert( x.oidMachine , "B1" )
assert.neq( x.oidMachine , y.oidMachine , "B2" )
assert.eq( x.oidMachine , y.oidMachineOld , "B3" )
