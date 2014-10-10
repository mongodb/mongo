// Check cases where index scans are aborted due to the collection being dropped.  SERVER-4400

t = db.jstests_queryoptimizer3;
t.drop();

p = startParallelShell( 'for( i = 0; i < 400; ++i ) { sleep( 50 ); db.jstests_queryoptimizer3.drop(); }' );

for( i = 0; i < 100; ++i ) {
    t.drop();
    t.ensureIndex({a:1});
    t.ensureIndex({b:1});
    for( j = 0; j < 100; ++j ) {
        t.save({a:j,b:j});
    }
    m = i % 5;
    if ( m == 0 ) {
        t.count({a:{$gte:0},b:{$gte:0}});        
    }
    else if ( m == 1 ) {
        t.find({a:{$gte:0},b:{$gte:0}}).itcount();
    }
    else if ( m == 2 ) {
        t.remove({a:{$gte:0},b:{$gte:0}});
    }
    else if ( m == 3 ) {
        t.update({a:{$gte:0},b:{$gte:0}},{});
    }
    else if ( m == 4 ) {
        t.distinct('x',{a:{$gte:0},b:{$gte:0}});
    }
}

p();
