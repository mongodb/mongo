# Fuzzing a Custom Sequence Producer Plugin
This directory contains example code for using a custom sequence producer in the zstd fuzzers.

You can build and run the code in this directory using these commands:
```
$ make corpora
$ make -C seq_prod_fuzz_example/
$ python3 ./fuzz.py build all --enable-fuzzer --enable-asan --enable-ubsan --cc clang --cxx clang++ --custom-seq-prod=seq_prod_fuzz_example/example_seq_prod.o
$ python3 ./fuzz.py libfuzzer simple_round_trip
```

See `../fuzz_third_party_seq_prod.h` and `../README.md` for more information on zstd fuzzing.
