
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
assert.eq( ip.version, 6, "1105");
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

ip = IpAddr("FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF/1")
assert.eq( ip.network, 'IpAddr("8000:0000:0000:0000:0000:0000:0000:0000")', 1200 )
ip.mask = 15
assert.eq( ip.network, 'IpAddr("FFFE:0000:0000:0000:0000:0000:0000:0000")', 1210 )
ip.mask = 16
assert.eq( ip.network, 'IpAddr("FFFF:0000:0000:0000:0000:0000:0000:0000")', 1220 )
ip.mask = 17
assert.eq( ip.network, 'IpAddr("FFFF:8000:0000:0000:0000:0000:0000:0000")', 1230 )
ip.mask = 63
assert.eq( ip.network, 'IpAddr("FFFF:FFFF:FFFF:FFFE:0000:0000:0000:0000")', 1240 )
ip.mask = 64
assert.eq( ip.network, 'IpAddr("FFFF:FFFF:FFFF:FFFF:0000:0000:0000:0000")', 1250 )
ip.mask = 65
assert.eq( ip.network, 'IpAddr("FFFF:FFFF:FFFF:FFFF:8000:0000:0000:0000")', 1260 )
ip.mask = 127
assert.eq( ip.network, 'IpAddr("FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFE")', 1270 )

ip = IpAddr("0.0.0.0/8")
assert.eq( ip.broadcast, 'IpAddr("0.255.255.255")', 1300 )
ip.mask = 9
assert.eq( ip.broadcast, 'IpAddr("0.127.255.255")', 1310 )
ip.mask = 15
assert.eq( ip.broadcast, 'IpAddr("0.1.255.255")', 1320 )
ip.mask = 16
assert.eq( ip.broadcast, 'IpAddr("0.0.255.255")', 1330 )
ip.mask = 17
assert.eq( ip.broadcast, 'IpAddr("0.0.127.255")', 1340 )
ip.mask = 23
assert.eq( ip.broadcast, 'IpAddr("0.0.1.255")', 1350 )
ip.mask = 24
assert.eq( ip.broadcast, 'IpAddr("0.0.0.255")', 1360 )
ip.mask = 25
assert.eq( ip.broadcast, 'IpAddr("0.0.0.127")', 1370 )
ip.mask = 31
assert.eq( ip.broadcast, 'IpAddr("0.0.0.1")', 1380 )

ip = IpAddr("255.255.255.255/8")
assert.eq( ip.network, 'IpAddr("255.0.0.0")', 1400 )
ip.mask = 9
assert.eq( ip.network, 'IpAddr("255.128.0.0")', 1410 )
ip.mask = 15
assert.eq( ip.network, 'IpAddr("255.254.0.0")', 1420 )
ip.mask = 16
assert.eq( ip.network, 'IpAddr("255.255.0.0")', 1430 )
ip.mask = 17
assert.eq( ip.network, 'IpAddr("255.255.128.0")', 1440 )
ip.mask = 23
assert.eq( ip.network, 'IpAddr("255.255.254.0")', 1450 )
ip.mask = 24
assert.eq( ip.network, 'IpAddr("255.255.255.0")', 1460 )
ip.mask = 25
assert.eq( ip.network, 'IpAddr("255.255.255.128")', 1470 )
ip.mask = 31
assert.eq( ip.network, 'IpAddr("255.255.255.254")', 1480 )


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

// All IPv4 addresses are less than any IPv6 addresses
ip = IpAddr("0000:0000:0000:0000:0000:0000:0000:0000")
ra = t.find( {ip: {$lt: ip}}).sort({ip: 1}).toArray()
assert.eq( 13, ra.length, "2100" );
assert.eq( ra[0].ip, 'IpAddr("0.0.0.0")', "2110" );
assert.eq( ra[1].ip, 'IpAddr("1.1.1.1")', "2120" );
assert.eq( ra[2].ip, 'IpAddr("1.2.3.4/16")', "2130" );
assert.eq( ra[3].ip, 'IpAddr("128.128.128.128")', "2140" );
assert.eq( ra[4].ip, 'IpAddr("192.168.1.27")', "2150" );
assert.eq( ra[5].ip, 'IpAddr("192.168.1.33")', "2160" );
assert.eq( ra[6].ip, 'IpAddr("255.0.0.0")', "2170" );
assert.eq( ra[7].ip, 'IpAddr("255.1.1.1")', "2180" );
assert.eq( ra[8].ip, 'IpAddr("255.255.0.0")', "2190" );
assert.eq( ra[9].ip, 'IpAddr("255.255.1.1")', "2200" );
assert.eq( ra[10].ip, 'IpAddr("255.255.255.0")', "2210" );
assert.eq( ra[11].ip, 'IpAddr("255.255.255.1")', "2220" );
assert.eq( ra[12].ip, 'IpAddr("255.255.255.255")', "2230" );


