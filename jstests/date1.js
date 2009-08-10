
t = db.date1;


function go( d , msg ){
    t.drop();
    t.save( { a : 1 , d : d } );
    assert.eq( d , t.findOne().d , msg )
}

go( new Date() , "A" )
go( new Date( 1 ) , "B")
go( new Date( 0 ) , "C (old spidermonkey lib fails this test)")

