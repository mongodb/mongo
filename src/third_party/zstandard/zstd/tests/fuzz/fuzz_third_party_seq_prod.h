/*
 * Copyright (c) Yann Collet, Meta Platforms, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef EXAMPLE_SEQ_PROD_H
#define EXAMPLE_SEQ_PROD_H

#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* *** INTERFACE FOR FUZZING THIRD-PARTY SEQUENCE PRODUCER PLUGINS ***
 * Fuzz-testing for the external sequence producer API was introduced in PR #3437.
 * However, the setup in #3437 only allows fuzzers to exercise the implementation of the
 * API itself (the code in the core zstd library which interacts with your plugin).
 *
 * This header defines an interface for plugin authors to link their code into the fuzzer
 * build. Plugin authors can provide an object file implementing the symbols below,
 * and those symbols will replace the default ones provided by #3437.
 *
 * To fuzz your plugin, follow these steps:
 *   - Build your object file with a recent version of clang. Building with gcc is not supported.
 *   - Build your object file using appropriate flags for fuzzing. For example:
 *     `-g -fno-omit-frame-pointer -fsanitize=undefined,address,fuzzer`
 *   - Build the fuzzer binaries with options corresponding to the flags you chose. Use --custom-seq-prod= to pass in your object file:
 *     `./fuzz.py build all --enable-fuzzer --enable-asan --enable-ubsan --cc clang --cxx clang++ --custom-seq-prod=your_object.o`
 *
 * An example implementation of this header is provided at tests/fuzz/seq_prod_fuzz_example/.
 * Use these commands to fuzz with the example code:
 *   $ make corpora
 *   $ make -C seq_prod_fuzz_example/
 *   $ python3 ./fuzz.py build all --enable-fuzzer --enable-asan --enable-ubsan --cc clang --cxx clang++ --custom-seq-prod=seq_prod_fuzz_example/example_seq_prod.o
 *   $ python3 ./fuzz.py libfuzzer simple_round_trip
 */

/* The fuzzer will call this function before each test-case. It should run any
 * setup actions (such as starting a hardware device) needed for fuzzing.
 *
 * The fuzzer will assert() that the return value is zero. To signal an error,
 * please return a non-zero value. */
size_t FUZZ_seqProdSetup(void);

/* The fuzzer will call this function after each test-case. It should free
 * resources aquired by FUZZ_seqProdSetup() to prevent leaks across test-cases.
 *
 * The fuzzer will assert() that the return value is zero. To signal an error,
 * please return a non-zero value. */
size_t FUZZ_seqProdTearDown(void);

/* The fuzzer will call this function before each test-case, only after calling
 * FUZZ_seqProdSetup(), to obtain a sequence producer state which can be passed
 * into ZSTD_registerSequenceProducer().
 *
 * All compressions which are part of a test-case will share a single sequence
 * producer state. Sharing the state object is safe because the fuzzers currently
 * don't exercise the sequence producer API in multi-threaded scenarios. We may
 * need a new approach in the future to support multi-threaded fuzzing.
 *
 * The fuzzer will assert() that the return value is not NULL. To signal an error,
 * please return NULL. */
void* FUZZ_createSeqProdState(void);

/* The fuzzer will call this function after each test-case. It should free any
 * resources aquired by FUZZ_createSeqProdState().
 *
 * The fuzzer will assert() that the return value is zero. To signal an error,
 * please return a non-zero value. */
size_t FUZZ_freeSeqProdState(void* sequenceProducerState);

/* This is the sequence producer function you would like to fuzz! It will receive
 * the void* returned by FUZZ_createSeqProdState() on each invocation. */
size_t FUZZ_thirdPartySeqProd(void* sequenceProducerState,
                              ZSTD_Sequence* outSeqs, size_t outSeqsCapacity,
                              const void* src, size_t srcSize,
                              const void* dict, size_t dictSize,
                              int compressionLevel,
                              size_t windowSize);

/* These macros are internal helpers. You do not need to worry about them. */
#ifdef FUZZ_THIRD_PARTY_SEQ_PROD
#define FUZZ_SEQ_PROD_SETUP()                                                  \
  do {                                                                         \
    FUZZ_ASSERT(FUZZ_seqProdSetup() == 0);                                     \
    FUZZ_seqProdState = FUZZ_createSeqProdState();                             \
    FUZZ_ASSERT(FUZZ_seqProdState != NULL);                                    \
  } while (0)
#else
#define FUZZ_SEQ_PROD_SETUP()
#endif

#ifdef FUZZ_THIRD_PARTY_SEQ_PROD
#define FUZZ_SEQ_PROD_TEARDOWN()                                               \
  do {                                                                         \
    FUZZ_ASSERT(FUZZ_freeSeqProdState(FUZZ_seqProdState) == 0);                \
    FUZZ_ASSERT(FUZZ_seqProdTearDown() == 0);                                  \
  } while (0)
#else
#define FUZZ_SEQ_PROD_TEARDOWN()
#endif

#ifdef __cplusplus
}
#endif

#endif /* EXAMPLE_SEQ_PROD_H */
