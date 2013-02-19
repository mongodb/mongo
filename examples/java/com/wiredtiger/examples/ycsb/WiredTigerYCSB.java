/**
 */
package com.wiredtiger.examples.ycsb;

import com.yahoo.ycsb.DB;
import com.yahoo.ycsb.DBException;
import com.yahoo.ycsb.ByteIterator;
import com.yahoo.ycsb.StringByteIterator;
import com.yahoo.ycsb.Utils;

import java.util.HashMap;
import java.util.Properties;
import java.util.Set;
import java.util.Enumeration;
import java.util.Vector;


/**
 * WiredTiger YCSB implementation.
 */
public class WiredTigerYCSB extends DB
{
	public static final String VERBOSE="WiredTiger.verbose";
	public static final String VERBOSE_DEFAULT="false";
	
	boolean verbose;
	int todelay;

	public WiredTigerYCSB()
	{
		todelay=0;
	}

	
	void delay()
	{
		if (todelay>0)
		{
			try
			{
				Thread.sleep((long)Utils.random().nextInt(todelay));
			}
			catch (InterruptedException e)
			{
				//do nothing
			}
		}
	}

	/**
	 * Initialize any state for this DB.
	 * Called once per DB instance; there is one DB instance per client thread.
	 */
	@SuppressWarnings("unchecked")
	public void init()
	{
		verbose=Boolean.parseBoolean(getProperties().getProperty(VERBOSE, VERBOSE_DEFAULT));
		todelay=0;
		
		if (verbose)
		{
			System.out.println("***************** properties *****************");
			Properties p=getProperties();
			if (p!=null)
			{
				for (Enumeration e=p.propertyNames(); e.hasMoreElements(); )
				{
					String k=(String)e.nextElement();
					System.out.println("\""+k+"\"=\""+p.getProperty(k)+"\"");
				}
			}
			System.out.println("**********************************************");
		}
	}

	/**
	 * Read a record from the database. Each field/value pair from the result will be stored in a HashMap.
	 *
	 * @param table The name of the table
	 * @param key The record key of the record to read.
	 * @param fields The list of fields to read, or null for all of them
	 * @param result A HashMap of field/value pairs for the result
	 * @return Zero on success, a non-zero error code on error
	 */
	public int read(String table, String key, Set<String> fields, HashMap<String,ByteIterator> result)
	{
		delay();

		if (verbose)
		{
			System.out.print("READ "+table+" "+key+" [ ");
			if (fields!=null)
			{
				for (String f : fields)
				{
					System.out.print(f+" ");
				}
			}
			else
			{
				System.out.print("<all fields>");
			}

			System.out.println("]");
		}

		return 0;
	}
	
	/**
	 * Perform a range scan for a set of records in the database. Each field/value pair from the result will be stored in a HashMap.
	 *
	 * @param table The name of the table
	 * @param startkey The record key of the first record to read.
	 * @param recordcount The number of records to read
	 * @param fields The list of fields to read, or null for all of them
	 * @param result A Vector of HashMaps, where each HashMap is a set field/value pairs for one record
	 * @return Zero on success, a non-zero error code on error
	 */
	public int scan(String table, String startkey, int recordcount, Set<String> fields, Vector<HashMap<String,ByteIterator>> result)
	{
		delay();

		if (verbose)
		{
			System.out.print("SCAN "+table+" "+startkey+" "+recordcount+" [ ");
			if (fields!=null)
			{
				for (String f : fields)
				{
					System.out.print(f+" ");
				}
			}
			else
			{
				System.out.print("<all fields>");
			}

			System.out.println("]");
		}

		return 0;
	}

	/**
	 * Update a record in the database. Any field/value pairs in the specified values HashMap will be written into the record with the specified
	 * record key, overwriting any existing values with the same field name.
	 *
	 * @param table The name of the table
	 * @param key The record key of the record to write.
	 * @param values A HashMap of field/value pairs to update in the record
	 * @return Zero on success, a non-zero error code on error
	 */
	public int update(String table, String key, HashMap<String,ByteIterator> values)
	{
		delay();

		if (verbose)
		{
			System.out.print("UPDATE "+table+" "+key+" [ ");
			if (values!=null)
			{
				for (String k : values.keySet())
				{
					System.out.print(k+"="+values.get(k)+" ");
				}
			}
			System.out.println("]");
		}

		return 0;
	}

	/**
	 * Insert a record in the database. Any field/value pairs in the specified values HashMap will be written into the record with the specified
	 * record key.
	 *
	 * @param table The name of the table
	 * @param key The record key of the record to insert.
	 * @param values A HashMap of field/value pairs to insert in the record
	 * @return Zero on success, a non-zero error code on error
	 */
	public int insert(String table, String key, HashMap<String,ByteIterator> values)
	{
		delay();

		if (verbose)
		{
			System.out.print("INSERT "+table+" "+key+" [ ");
			if (values!=null)
			{
				for (String k : values.keySet())
				{
					System.out.print(k+"="+values.get(k)+" ");
				}
			}

			System.out.println("]");
		}

		return 0;
	}


	/**
	 * Delete a record from the database. 
	 *
	 * @param table The name of the table
	 * @param key The record key of the record to delete.
	 * @return Zero on success, a non-zero error code on error
	 */
	public int delete(String table, String key)
	{
		delay();

		if (verbose)
		{
			System.out.println("DELETE "+table+" "+key);
		}

		return 0;
	}

	/**
	 * Short test of WiredTigerYCSB
	 */
	/*
	public static void main(String[] args)
	{
		WiredTigerYCSB bdb=new WiredTigerYCSB();

		Properties p=new Properties();
		p.setProperty("Sky","Blue");
		p.setProperty("Ocean","Wet");

		bdb.setProperties(p);

		bdb.init();

		HashMap<String,String> fields=new HashMap<String,String>();
		fields.put("A","X");
		fields.put("B","Y");

		bdb.read("table","key",null,null);
		bdb.insert("table","key",fields);

		fields=new HashMap<String,String>();
		fields.put("C","Z");

		bdb.update("table","key",fields);

		bdb.delete("table","key");
	}*/
}
