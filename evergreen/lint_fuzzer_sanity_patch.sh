DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o pipefail
set -o verbose

activate_venv

mkdir -p jstestfuzzinput jstestfuzzoutput

# We need to be the jstestfuzz repo for node to install/run
cd jstestfuzz

indir="$(pwd)/../jstestfuzzinput"
outdir="$(pwd)/../jstestfuzzoutput"

# Grep all the js files from modified_and_created_patch_files.txt and put them into $indir.
(grep -v "\.tpl\.js$" ../modified_and_created_patch_files.txt | grep ".*jstests/.*\.js$" | xargs -I {} cp {} $indir || true)

# Count the number of files in $indir.
if [[ "$(ls -A $indir)" ]]; then
  num_files=$(ls -A $indir | wc -l)

  # Only fetch 50 files to generate jsfuzz testing files.
  if [[ $num_files -gt 50 ]]; then
    num_files=50
  fi

  ./src/scripts/npm_run.sh jstestfuzz -- --jsTestsDir $indir --out $outdir --numSourceFiles $num_files --numGeneratedFiles 50

  # Run parse-jsfiles on 50 files at a time with 32 processes in parallel.
  ls -1 -d $outdir/* | xargs -P 32 -L 50 ./src/scripts/npm_run.sh parse-jsfiles -- | tee lint_fuzzer_sanity.log
  exit_code=$?

  # Exit out of the jstestfuzz directory
  cd ..
  $python ./buildscripts/simple_report.py --test-name lint_fuzzer_sanity_patch --log-file jstestfuzz/lint_fuzzer_sanity.log --exit-code $exit_code
fi
