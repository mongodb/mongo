o = new ObjectId();
assert(o.getTimestamp);

a = new ObjectId("4c17f616a707427266a2801a");
b = new ObjectId("4c17f616a707428966a2801c");
assert.eq(a.getTimestamp(), b.getTimestamp() , "A" );

x = Math.floor( (new Date()).getTime() / 1000 );
sleep(10/*ms*/)
a = new ObjectId();
sleep(10/*ms*/)
z = Math.floor( (new Date()).getTime() / 1000 );
y = a.getTimestamp().getTime() / 1000;

assert.lte( x , y , "B" );
assert.lte( y , z , "C" );
