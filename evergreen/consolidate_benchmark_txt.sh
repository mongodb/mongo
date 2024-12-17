DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

OUTPUT_FILE="build/benchmarks.txt"

# Concatenate all text files in the directory into the output file
for file in build/*_bm.txt; do
  cat "$file" >> "$OUTPUT_FILE"
done
