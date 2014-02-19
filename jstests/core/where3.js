
t = db.where3;
t.drop()

t.save( { returned_date : 5 } );
t.save( { returned_date : 6 } );

assert.eq( 1 , t.find( function(){ return this.returned_date == 5; } ).count() , "A" );
assert.eq( 1 , t.find( { $where : "return this.returned_date == 5;" } ).count() , "B" );
assert.eq( 1 , t.find( { $where : "this.returned_date == 5;" } ).count() , "C" );
