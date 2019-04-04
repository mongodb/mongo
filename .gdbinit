# Print the full stack trace on python exceptions to aid debugging
set python print-stack full

# Load the merizodb utilities
source buildscripts/gdb/merizo.py

# Load the merizodb pretty printers
source buildscripts/gdb/merizo_printers.py

# Load the merizodb lock analysis
source buildscripts/gdb/merizo_lock.py
