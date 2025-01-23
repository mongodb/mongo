Actions taken to create this directory:

1. Create a new directory
2. Create a bash script for easy repeatability of steps
   1. This should clear the a temporary directory
   2. Download the code needed to build
   3. Generate any platform specific files
   4. Make it easy to copy over needed files to the new directory and delete any unneceesary files
3. Make a `BUILD.bazel` script
   1. To figure out all the targets needed I ran the `cmake --build` against a build directory
   2. The AWS SDK generated a bunch of cmake files and directories. Importannt ones include: `CMakeFiles`, `<pkg>.dir`, `flages.make`, `includes_CXX.rsp`, and `objects1.rsp`.
   3. Converted the information found in these files into our `mongo_cc_library`.
   4. Created a unified target for importing into other targets.
4. Created some other runnable target (like a unit test). To actually see if everything was building/linking/and running as expected.
