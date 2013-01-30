
a = db.dbref2a;
b = db.dbref2b;

a.drop();
b.drop();

a.save( { name : "eliot" } );
b.save( { num : 1 , link : new DBRef( "dbref2a" , a.findOne()._id ) } );
assert.eq( "eliot" , b.findOne().link.fetch().name , "A" );
assert.neq( "el" , b.findOne().link.fetch().name , "B" );
