a = fork( function( a, b ) { return a / b; }, 10, 2 );
a.start();
b = fork( function( a, b, c ) { return a + b + c; }, 18, " is a ", "multiple of 3" );
makeFunny = function( text ) {
    return text + " ha ha!";
}
c = fork( makeFunny, "paisley" );
c.start();
b.start();
b.join();
assert.eq( 5, a.returnData() );
assert.eq( "18 is a multiple of 3", b.returnData() );
assert.eq( "paisley ha ha!", c.returnData() );

z = fork( function( a ) {
	var y = fork( function( a ) {
		return a + "b"; }, "a" );
	y.start();
	return y.returnData() + a;
    }, "c" );
z.start();
assert.eq( "abc", z.returnData() );
