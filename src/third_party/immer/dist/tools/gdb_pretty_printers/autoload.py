import gdb.printing
import os

path = os.path.dirname(__file__)
if not path in sys.path:
    sys.path.append(path)
from printers import immer_lookup_function

gdb.printing.register_pretty_printer(gdb.current_objfile(), immer_lookup_function)

print("immer gdb pretty-printers loaded")