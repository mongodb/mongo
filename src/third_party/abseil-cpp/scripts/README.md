# Updating abseil

The SConscript file for the abseil build is parsed via the generated ninja 
file from the native abseil cmake build. The parse_libs_from_ninja.py will 
perform the parsing and generation of the SConscript file. 

To update abseil you should:

1. update the version/repo information in the import.py script
2. remove the existing abseil-cpp/dist directory
3. run the import.py script
4. run the parse_libs_from_ninja.py

# Updating the SConscript generated libraries

The parse_libs_from_ninja.py will extract specifically requested libraries 
from the native abseil build. This list of libraries should be a python list 
defined near the top fo the parse_libs_from_ninja.py file. 
Update this list if a new set of libraries is required. 

Note that the script works by iteration throught he list of specified libraries, 
finding the library one at a time in the ninja file, extracting source files and 
dependent libraries, then reiterating untill all specific and dependent libraries 
have been found. If a specified library can not be found the script will fail 
with exception.

# Mongodbtoolchain dependency 

Note that the parse_libs_from_ninja.py assumes the mongodb toolchain is installed 
and access cmake via the toolchain. If this is not the case you will need to 
update the script for you particuler environment.

