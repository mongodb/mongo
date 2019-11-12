#!/bin/bash
# merge_corpus.sh
#
# Merges the corpus of each libfuzzer tests

set -o verbose
set -o errexit

input="build/libfuzzer_tests.txt"
corpus_dir="corpus"

if [ ! -f $input ] || [ ! -d $corpus_dir ]; then
    echo "Missing corpus information"
    exit 0
fi

# For each test, merge in new data.
while IFS= read -r line; do
    corpus_file="$corpus_dir/corpus-${line##*/}"

    if [ -d "${corpus_file}-new" ]; then
        if [ ! -d "${corpus_file}" ]; then
            # An error in a prior run left old corpus data orphaned, reclaim it.
            mv -v "${corpus_file}-new" "${corpus_file}"
        else
            # Somehow we have multiple corpii, treat non '-new' as official.
            rm -rv "${corpus_file}-new"
        fi
    fi

    # Create a new merge from old corpus and new run.
    mkdir -v "${corpus_file}-new"
    ./"$line" "${corpus_file}-new" "$corpus_file" -merge=1
done < "$input"

# Delete old corpus.
find corpus/* -not -name '*-new' -type d -exec rm -rv {} +

# Rename new corpus to old corpus.
for f in "$corpus_dir"/*-new; do
    mv -v "$f" "${f%-new}"
done
