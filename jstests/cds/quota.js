// test read/write permissions


REMOTE_IP="192.168.22.135";

test = new Mongo( REMOTE_IP ).getDB("test");
test.auth("lockmind","123456");
for (i = 0 ; i < 1000000 ; i ++ ) {
	test.aaa.insert({"dd":i,"ss":"asddddddddddddddddddddddddddddddddddddddddddd"});
}

