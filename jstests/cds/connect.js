// test connect permissions

REMOTE_IP="192.168.22.135";

for(i = 0 ; i < 10 ; i ++) {
	t = new Mongo( REMOTE_IP );
	t.getDB( "test" ).auth("lockmind" , "123456");
}

