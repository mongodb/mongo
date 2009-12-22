t = db.jstests_parallel_basic;

// would be nice to configure and output a random seed so we can reproduce
// behavior.  Seems like that's impossible using Math.random().

expTimeout = function( mean ) {
    return -Math.log( Math.random() ) * mean;
}
