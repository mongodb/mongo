
t = db.server5441;
t.drop();

function checkgle() {
    var gle = db.getLastErrorObj();
    assert.eq( 2 , gle.n , tojson( gle ) );
}

t.insert( { x : 1 } );
t.insert( { x : 1 } );
updateReturn = t.update( {} , { $inc : { x : 2 } } , false , true );

for ( i=0; i<100; i++ ) 
    checkgle();

db.adminCommand( { replSetGetStatus : 1 , forShell : 1 } );
shellPrintHelper( updateReturn )
replSetMemberStatePrompt()

checkgle();
