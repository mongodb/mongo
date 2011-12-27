// Don't crash attempting to retry additional query plans if a recorded plan failed because a cursor was dropped.

t = db.jstests_queryoptimizer5;
t.drop();

function reset() {
    t.drop();
    for( var i = 0; i < 5000; ++i ) {
        t.save({a:i,b:i});
    }
    t.ensureIndex({a:1});
    t.ensureIndex({b:1});
}

s = startParallelShell( "for( i = 0; i < 30; ++i ) { sleep( 200 ); db.jstests_queryoptimizer5.drop(); }" );

for( var i = 0; i < 10; ++i ) {
    try {
        reset();
        t.find( {$or:[{a:{$gte:0},b:{$gte:0}}]} ).batchSize( 10000 ).itcount();
        t.find( {$or:[{a:{$gte:0},b:{$gte:0}}]} ).batchSize( 10000 ).itcount();
    } catch (e) {
//        printjson(e);
    }
}

s();
