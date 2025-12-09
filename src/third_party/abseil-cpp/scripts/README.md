# Updating abseil

The BUILD.bazel file for the abseil build is parsed via the generated ninja 
file from the native abseil cmake build. The parse_libs_from_ninja.py will 
perform the parsing and generation of the BUILD.bazel file. 

To update abseil you should:

1. update the version/repo information in the import.py script
2. remove the existing abseil-cpp/dist directory
3. run the import.sh script

# Mongodbtoolchain dependency 

Note that the parse_libs_from_ninja.py assumes the mongodb toolchain is installed 
and access cmake via the toolchain. If this is not the case you will need to 
update the script for you particuler environment.

