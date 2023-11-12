# Custom gdb scripts
Custom gdb scripts for gdb debugging and autoloading of these scripts.
Individual scripts are located in the `gdb_scripts/` and the top level directory is reserved for loader scripts `load_gdb_scripts.py`.

### Usage
To load scripts from within gdb call `source /path/to/load_gdb_scripts.py`.
If you have compiled WiredTiger with the `-DENABLE_SHARED=1` flag then these scripts 
will be auto-loaded when gdb is opened.

### Creating new scripts
Scripts can be written in either python ([hazard_pointers.py](./gdb_scripts/hazard_pointers.py)) or GDB command files ([dump_row_int.gdb](./gdb_scripts/dump_row_int.gdb)). If you create a new python script it is recommended you follow the pattern of `hazard_pointers.py` where a class that extends `gdb.Command` is created for each command. Don't forget to register the new command by calling its constructor at the bottom of the file.
Once created update `load_gdb_scripts.py` to include the new script.