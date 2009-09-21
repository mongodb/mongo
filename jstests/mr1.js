
t = db.mr1;
t.drop();

t.save( { x : 1 , tags : [ "a" , "b" ] } );
t.save( { x : 2 , tags : [ "b" , "c" ] } );
t.save( { x : 3 , tags : [ "c" , "a" ] } );
t.save( { x : 4 , tags : [ "b" , "c" ] } );

emit = printjson;

m = function(){
    this.tags.forEach(
        function(z){
            emit( z , { count : 1 } );
        }
    );
};

r = function( key , values ){
    var total = 0;
    for ( var i=0; i<values.length; i++ )
        total += values[i].count;
    return { count : total };
};

res = db.runCommand( { mapreduce : "mr1" , map : m , reduce : r } );
printjson( res );

print( Array.tojson( db[res.result].find().toArray() , "\n" ) );


