t = db.sort4;
t.drop();


function nice( sort , correct , extra ){
    var c = t.find().sort( sort );
    var s = "";
    c.forEach( 
        function(z){
            if ( s.length )
                s += ",";
            s += z.name;
            if ( z.prename )
                s += z.prename;
        }
    );
    print( tojson( sort ) + "\t" + s );
    if ( correct )
        assert.eq( correct , s , tojson( sort ) + "(" + extra + ")" );
    return s;
}

t.save({name: 'A', prename: 'B'})
t.save({name: 'A', prename: 'C'})
t.save({name: 'B', prename: 'B'})
t.save({name: 'B', prename: 'D'})

nice( { name:1 } , "AB,AC,BB,BD" , "s1" );
nice( { prename : 1 } , "AB,BB,AC,BD" , "s2" );
nice( {name:1, prename:1} , "AB,AC,BB,BD" , "s3" );

t.save({name: 'A'})     
nice( {name:1, prename:1} , "A,AB,AC,BB,BD" , "e1" );          

t.save({name: 'C'})               
nice( {name:1, prename:1} , "A,AB,AC,BB,BD,C" , "e2" ); // SERVER-282

t.ensureIndex( { name : 1 , prename : 1 } );
nice( {name:1, prename:1} , "A,AB,AC,BB,BD,C" , "e2ia" ); // SERVER-282

t.dropIndexes();
t.ensureIndex( { name : 1  } );
nice( {name:1, prename:1} , "A,AB,AC,BB,BD,C" , "e2ib" ); // SERVER-282
