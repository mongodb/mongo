// test change user

REMOTE_IP="192.168.22.135";

conn=new Mongo( REMOTE_IP);
conn.getDB("test").auth("lockmind","123456");
conn.getDB("test1").auth("lockmind","123456"); 


conn2 = new Mongo(REMOTE_IP);
db=conn2.getDB("admin");
db.auth("sa","sa");
conn2.getDB("test").getCollection("system.users").find();
conn2.getDB("test1").getCollection("system.users").find();
