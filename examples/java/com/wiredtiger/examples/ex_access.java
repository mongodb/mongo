package com.wiredtiger.examples;
import com.wiredtiger.db.*;

public class ex_access {
	public static void main(String[] args) {
		Connection conn = wiredtiger.open("WT_HOME", "create");
		Session s = conn.open_session(null);
		s.create("table:t", "key_format=S,value_format=u");
		Cursor c = s.open_cursor("table:t", null, null);
		c.set_key("foo");
		c.set_value("bar".getBytes());
		c.insert();
		c.reset();
		while (c.next() == 0)
			System.out.println("Got: " + c.get_key());
		conn.close(null);
	}
}
