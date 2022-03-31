set -o errexit
set -o verbose

cd src
# TODO SERVER-49884 Remove this when we no longer check in generated Bison.
# Here we use the -header-filter option to instruct clang-tidy to scan our header files. The
# regex instructs clang-tidy to scan headers in our source directory with the mongo/* regex, and
# the build directory to analyze generated headers with the build/* regex
BISON_GENERATED_PATTERN=parser_gen\.cpp
jq -r '.[] | .file' compile_commands.json \
  | grep src/mongo \
  | grep -v $BISON_GENERATED_PATTERN \
  | xargs -n 32 -P $(grep -c ^processor /proc/cpuinfo) -t \
    /opt/mongodbtoolchain/v3/bin/clang-tidy \
    -p ./compile_commands.json
