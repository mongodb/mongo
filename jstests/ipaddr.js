
t = db.ip_addr_test
t.drop()

var ip1 = IpAddr( "192.168.1.1" );
assert.eq( ip1, 'IpAddr("192.168.1.1")', "1000");
assert.eq( ip1.toString(), 'IpAddr("192.168.1.1")', "1010");
assert.eq( ip1.tojson(), 'IpAddr("192.168.1.1")', "1020");
assert.eq( ip1.mask, '32', "1030");

ip1 = IpAddr( "192.168.1.1/16" );
assert.eq( ip1, 'IpAddr("192.168.1.1/16")', "1040");
assert.eq( ip1.mask, '16', "1050");

ip1.mask = 15;
assert.eq( ip1, 'IpAddr("192.168.1.1/15")', "1060");
assert.eq( ip1.mask, '15', "1070");
assert.eq( ip1.version, 4, "1080");

// Test netmasks with broadcast and network
ip = IpAddr("::/1")
assert.eq( ip.broadcast, 'IpAddr("7FFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF")', 1100 )
ip.mask = 15
assert.eq( ip.broadcast, 'IpAddr("0001:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF")', 1110 )
ip.mask = 16
assert.eq( ip.broadcast, 'IpAddr("0000:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF")', 1120 )
ip.mask = 17
assert.eq( ip.broadcast, 'IpAddr("0000:7FFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF")', 1130 )
ip.mask = 63
assert.eq( ip.broadcast, 'IpAddr("0000:0000:0000:0001:FFFF:FFFF:FFFF:FFFF")', 1140 )
ip.mask = 64
assert.eq( ip.broadcast, 'IpAddr("0000:0000:0000:0000:FFFF:FFFF:FFFF:FFFF")', 1150 )
ip.mask = 65
assert.eq( ip.broadcast, 'IpAddr("0000:0000:0000:0000:7FFF:FFFF:FFFF:FFFF")', 1160 )
ip.mask = 127
assert.eq( ip.broadcast, 'IpAddr("0000:0000:0000:0000:0000:0000:0000:0001")', 1170 )


t.drop()
t.insert( { ip: IpAddr("0.0.0.0") } )
t.insert( { ip: IpAddr("1.1.1.1") } )
t.insert( { ip: IpAddr("1.2.3.4/16") } )
t.insert( { ip: IpAddr("128.128.128.128") } )
t.insert( { ip: IpAddr("192.168.1.27") } )
t.insert( { ip: IpAddr("192.168.1.33") } )
t.insert( { ip: IpAddr("255.0.0.0") } )
t.insert( { ip: IpAddr("255.1.1.1") } )
t.insert( { ip: IpAddr("255.255.0.0") } )
t.insert( { ip: IpAddr("255.255.1.1") } )
t.insert( { ip: IpAddr("255.255.255.0") } )
t.insert( { ip: IpAddr("255.255.255.1") } )
t.insert( { ip: IpAddr("255.255.255.255") } )
t.insert( { ip: IpAddr("0000:0000:0000:0000:0000:0000:0000:0000") } )
t.insert( { ip: IpAddr("FFFF:0000:0000:0000:0000:0000:0000:0000") } )
t.insert( { ip: IpAddr("FFFF:FFFF:0000:0000:0000:0000:0000:0000") } )
t.insert( { ip: IpAddr("FFFF:FFFF:FFFF:0000:0000:0000:0000:0000") } )
t.insert( { ip: IpAddr("FFFF:FFFF:FFFF:FFFF:0000:0000:0000:0000") } )
t.insert( { ip: IpAddr("FFFF:FFFF:FFFF:FFFF:FFFF:0000:0000:0000") } )
t.insert( { ip: IpAddr("FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:0000:0000") } )
t.insert( { ip: IpAddr("FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:0000") } )
t.insert( { ip: IpAddr("FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF") } )

ra = t.find( { ip: { $lt: IpAddr("0.0.0.0") } } ).sort({ip: 1}).toArray()
assert.eq( 0, ra.length, "2000" );

ra = t.find( { ip: { $lte: IpAddr("0.0.0.0") } } ).sort({ip: 1}).toArray()
assert.eq( 1, ra.length, "2010" );
assert.eq( ra[0].ip, 'IpAddr("0.0.0.0")', "2030" );

ip = IpAddr("192.168.1.0/24")
ra = t.find( {ip: {$gt: ip.network, $lt: ip.broadcast}}).sort({ip: 1}).toArray()
assert.eq( 2, ra.length, "2040" );
assert.eq( ra[0].ip, 'IpAddr("192.168.1.27")', "2050" );
assert.eq( ra[1].ip, 'IpAddr("192.168.1.33")', "2060" );

