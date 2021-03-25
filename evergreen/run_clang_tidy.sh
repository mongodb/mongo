set -o errexit
set -o verbose

cd src
# TODO SERVER-49884 Remove this when we no longer check in generated Bison.
BISON_GENERATED_PATTERN=parser_gen\.cpp
jq -r '.[] | .file' compile_commands.json \
  | grep src/mongo \
  | grep -v $BISON_GENERATED_PATTERN \
  | xargs -n 32 -P $(grep -c ^processor /proc/cpuinfo) -t \
    /opt/mongodbtoolchain/v3/bin/clang-tidy \
    -p ./compile_commands.json \
    --checks="-*,bugprone-unused-raii,bugprone-use-after-move,readability-const-return-type,readability-avoid-const-params-in-decls" \
    -warnings-as-errors="*"
