# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

import gdb, sys
 
# This script walks an insert list in WiredTiger, it does so by traversing the bottom level of the
# skip list and printing the next pointer array. It dumps the data to "dump.txt" as the content
# is often too large for the terminal.
#
# It takes a WT_INSERT_HEAD structure and a key format. In order to use it in gdb it must first be
# sourced. Example usage:
# source dump_insert_list.py
# dump_insert_list WT_INSERT_HEAD,S
#
# It supports 3 key formats: u, i, S.
class insert():
    def __init__(self, key, address, next_array):
        self.key = key
        self.address = address
        self.next_array = next_array
 
    def print(self, output_file):
        output_file.write("Key: " + str(self.key) + " Address: " + self.address + " Next array: " + str(self.next_array) + "\n")
 
class dump_insert_list(gdb.Command):
    key_format = 'S'
    inserts = {}
    def __init__(self):
        super(dump_insert_list, self).__init__("dump_insert_list", gdb.COMMAND_DATA)

    def usage(self):
        print("usage:")
        print("dump_insert_list WT_INSERT_HEAD,key_format")
        sys.exit(1)

    def decode_key(self, key):
        if self.key_format == 'S':
            return(key.decode())
        elif self.key_format == 'u':
            return(key)
        elif self.key_format == 'i':
            return(key)
        else:
            print(f"Invalid key format supplied: {self.key_format}")
            self.usage()
        
    def get_key(self, insert):
        key_struct = insert['u']['key']
        key = gdb.selected_inferior().read_memory(int(insert) + key_struct['offset'], key_struct['size']).tobytes()
        decoded_key = self.decode_key(key)
        return decoded_key
 
    def walk_level(self, head, level, output_file):
        current = head[level]
        while current != 0x0:
            key = self.get_key(current)
            next = []
            for i in range(0, 10):
                next.append(str(current['next'][i]))
                if (str(current['next'][i]) == "0x0"):
                    break
            self.inserts.update({key : insert(key, str(current), next)})
            self.inserts[key].print(output_file)
            current = current['next'][level]
 
    def invoke(self, args, from_tty):
        # Clear the data.
        self.inserts = {}
        # Initialize the output file.
        with open("dump.txt", "w") as output_file:
            arg_array = args.split(',')
            self.key_format = arg_array[1].strip()
            print("Parsing the passed in WT_INSERT_HEAD...")
            wt_insert_head = gdb.parse_and_eval(arg_array[0])
            print("Walking the insert list...")
            self.walk_level(wt_insert_head['head'], 0, output_file)
            print("Complete. Details have been written to dump.txt")
 
# This registers our class to the gdb runtime at "source" time.
dump_insert_list()
