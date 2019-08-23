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

# We need to merge the corpus once it has been tested
while IFS= read -r line
do
    mkdir "$corpus_dir"/corpus-"${line##*/}"-new
    ./"$line" "$corpus_dir"/corpus-"${line##*/}"-new "$corpus_dir"/corpus-"${line##*/}" -merge=1
done < "$input"

# Delete old corpus
find corpus/* -not -name '*-new' -type d -exec rm -rv {} +

# Rename new corpus to old corpus
for f in ./corpus/*
do
    mv "$f" "${f%-new}"
done
