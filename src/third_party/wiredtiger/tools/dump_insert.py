import gdb

filename = "dump.txt"
f = open(filename, "w")
inserts = {}

class insert():
    def __init__(self, key, address, next_array):
        self.key = key
        self.address = address
        self.next_array = next_array

    def print(self):
        f.write("Key: " + self.key + " Address: " + self.address + " Next array: " + str(self.next_array) + "\n")

"""
To use this:

0. Consider turning off pagination:
   (gdb) set pagination off
1. Source the script, e.g.
   (gdb) source tools/dump_insert.py
2. Give it an insert head:
   (gdb) p head
   $1 = (WT_INSERT_HEAD **) 0x36a62050
   (gdb) insert_list_dump($1)
"""
class insert_list_dump(gdb.Command):
    def __init__(self):
        super(insert_list_dump, self).__init__("insert_list_dump", gdb.COMMAND_DATA)

    def get_key(self, insert):
        key_struct = insert['u']['key']
        key = gdb.selected_inferior().read_memory(int(insert) + key_struct['offset'], key_struct['size']).tobytes().replace(b'\x00',b'').decode()
        return key.strip('0')

    def walk_skiplist(self, head, id):
        current = head['head'][0]
        while current != 0x0:
            key = self.get_key(current)
            next = []
            depth = current['depth']
            for i in range(0,depth):
                next.append(str(current['next'][i]))
            inserts.update({key : insert(key, str(current), next)})
            inserts[key].print()
            current = current['next'][0]

    def invoke(self, insert_head, from_tty):
        head = gdb.parse_and_eval(insert_head).dereference().dereference()
        f.write(str(head)+ "\n")
        self.walk_skiplist(head, 0)
        print("Saved results to {}".format(filename))

# This registers our class with the gdb runtime at "source" time.
insert_list_dump()
