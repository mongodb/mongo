from wiredtiger import wiredtiger_open

# Connect to the database and open a session
conn = wiredtiger_open('WT_TEST', 'create')
session = conn.open_session()

# Create a simple table
session.create('table:T', 'key_format=S,value_format=S')

# Open a cursor and insert a record
cursor = session.open_cursor('table:T', None)

cursor.set_key('key1')
cursor.set_value('value1')
cursor.insert()

# Iterate through the records
cursor.reset()
for key, value in cursor:
    print('Got record: ' + key + ' : ' + value)

conn.close()
