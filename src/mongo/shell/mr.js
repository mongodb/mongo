// mr.js

MR = {};

MR.init = function(){
    $max = 0;
    $arr = [];
    emit = MR.emit;
    $numEmits = 0;
    $numReduces = 0;
    $numReducesToDB = 0;
    gc(); // this is just so that keep memory size sane
}

MR.cleanup = function(){
    MR.init();
    gc();
}

MR.emit = function(k,v){
    $numEmits++;
    var num = nativeHelper.apply( get_num_ , [ k ] );
    var data = $arr[num];
    if ( ! data ){
        data = { key : k , values : new Array(1000) , count : 0 };
        $arr[num] = data;
    }
    data.values[data.count++] = v;
    $max = Math.max( $max , data.count );
}

MR.doReduce = function( useDB ){
    $numReduces++;
    if ( useDB )
        $numReducesToDB++;
    $max = 0;
    for ( var i=0; i<$arr.length; i++){
        var data = $arr[i];
        if ( ! data ) 
            continue;
        
        if ( useDB ){
            var x = tempcoll.findOne( { _id : data.key } );
            if ( x ){
                data.values[data.count++] = x.value;
            }
        }

        var r = $reduce( data.key , data.values.slice( 0 , data.count ) );
        if ( r && r.length && r[0] ){ 
            data.values = r; 
            data.count = r.length;
        }
        else{ 
            data.values[0] = r;
            data.count = 1;
        }
        
        $max = Math.max( $max , data.count ); 
        
        if ( useDB ){
            if ( data.count == 1 ){
                tempcoll.save( { _id : data.key , value : data.values[0] } );
            }
            else {
                tempcoll.save( { _id : data.key , value : data.values.slice( 0 , data.count ) } );
            }
        }
    }
}

MR.check = function(){                        
    if ( $max < 2000 && $arr.length < 1000 ){ 
        return 0; 
    }
    MR.doReduce();
    if ( $max < 2000 && $arr.length < 1000 ){ 
        return 1;
    }
    MR.doReduce( true );
    $arr = []; 
    $max = 0; 
    reset_num();
    gc();
    return 2;
}

MR.finalize = function(){
    tempcoll.find().forEach( 
        function(z){
            z.value = $finalize( z._id , z.value );
            tempcoll.save( z );
        }
    );
}
