# Print the full stack trace on python exceptions to aid debugging
set python print-stack full

# Load the mongodb utilities
source buildscripts/gdb/mongo.py

# Load the mongodb pretty printers
source buildscripts/gdb/optimizer_printers.py
source buildscripts/gdb/mongo_printers.py

# Load the mongodb lock analysis
source buildscripts/gdb/mongo_lock.py

# Load methods for printing in-memory contents of WT tables.
source buildscripts/gdb/wt_dump_table.py

# Load third-party pretty printers
source src/third_party/immer/dist/tools/gdb_pretty_printers/autoload.py
