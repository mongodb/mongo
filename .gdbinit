# Print the full stack trace on python exceptions to aid debugging
set python print-stack full

# Load the mongodb utilities
source buildscripts/gdb/mongo.py

# Load the mongodb pretty printers
source buildscripts/gdb/mongo_printers.py

# Load the mongodb lock analysis
source buildscripts/gdb/mongo_lock.py

# Load third-party pretty printers
source src/third_party/immer/dist/tools/gdb_pretty_printers/autoload.py
