// mr.js

MR = {};

MR.init = function(){
    $max = 0;
    $arr = [];
    emit = MR.emit;
    $numEmits = 0;
    gc(); // this is just so that keep memory size sane
}

MR.cleanup = function(){
    MR.init();
    gc();
}

MR.emit = function(k,v){
    $numEmits++;
    var num = get_num( k );
    var data = $arr[num];
    if ( ! data ){
        data = { key : k , values : [] };
        $arr[num] = data;
    }
    data.values.push( v );
    $max = Math.max( $max , data.values.length );
}

MR.doReduce = function( useDB ){
    $max = 0;
    for ( var i=0; i<$arr.length; i++){
        var data = $arr[i];
        if ( ! data ) 
            continue;
        
        if ( useDB ){
            var x = tempcoll.findOne( { _id : data.key } );
            if ( x ){
                data.values.push( x.value );
            }
        }

        var r = $reduce( data.key , data.values );
        if ( r && r.length && r[0] ){ 
            data.values = r; 
        }
        else{ 
            data.values = [ r ]; 
        }
        
        $max = Math.max( $max , data.values.length ); 
        
        if ( useDB ){
            if ( data.values.length == 1 ){
                tempcoll.save( { _id : data.key , value : data.values[0] } );
            }
            else {
                tempcoll.save( { _id : data.key , value : data.values } );
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

