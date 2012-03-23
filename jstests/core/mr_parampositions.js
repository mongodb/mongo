
t = db.mr_replace;
t.drop();

t.insert( { a : [ 1 , 2 ] } )
t.insert( { a : [ 2 , 3 ] } )
t.insert( { a : [ 3 , 4 ] } )

outCollStr = "mr_replace_col";
outDbStr = "mr_db";

m = function(){ for (i=0; i<this.a.length; i++ ) emit( this.a[i] , 1 ); }
r = function(k,vs){ return Array.sum( vs ); }

function tos( o ){
    var s = "";
    for ( var i=0; i<100; i++ ){
        if ( o[i] )
            s += i + "_" + o[i];
    }
    return s;
}

positions = [{ replace: outCollStr, db: outDbStr },  {db: outDbStr, replace: outCollStr}];

for ( i= 0 ; i < 2; i++ ) {
  print("Testing mr replace into other DB")
  res = t.mapReduce( m , r , { out : positions[i] });
  printjson( res );
  expected = { "1" : 1 , "2" : 2 , "3" : 2 , "4" : 1 };
  outDb = db.getMongo().getDB(outDbStr);
  outColl = outDb[outCollStr];
  str = tos( outColl.convertToSingleObject("value") )
  print("Received result: " + str);
  assert.eq( tos( expected ) , str , "A Received wrong result " + str );
}

