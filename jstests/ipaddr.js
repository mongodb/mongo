
var ip1 = new IpAddr( "192.168.1.1" );
assert.eq( ip1, 'IpAddr("192.168.1.1")', "A");
assert.eq( ip1.toString(), 'IpAddr("192.168.1.1")', "A1");
assert.eq( ip1.tojson(), 'IpAddr("192.168.1.1")', "A2");
assert.eq( ip1.mask, '32', "B");

ip1 = new IpAddr( "192.168.1.1/16" );
assert.eq( ip1, 'IpAddr("192.168.1.1/16")', "C");
assert.eq( ip1.mask, '16', "D");

ip1.mask = 15;
assert.eq( ip1, 'IpAddr("192.168.1.1/15")', "E");
assert.eq( ip1.mask, '15', "F");
assert.eq( ip1.isIPv6, 0, "G");

