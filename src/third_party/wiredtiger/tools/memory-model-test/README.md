# WiredTiger Memory Model Test

The WiredTiger Memory Model Test performs a series of operations that, depending on the processor,
may generate out-of-order memory accesses that are visible to the code.

It is not a 'pass or fail' type of test. It is a technology demo and testbed for what can occur with
out-of-order memory accesses, and clearly shows in concrete form the differences in behaviour
between the ARM64 and x86_64 memory models.

This test was inspired, in part, by the work at https://preshing.com/20120515/memory-reordering-caught-in-the-act/, 
which is worth reading to understand these tests.

## The Tests

Memory Model Test runs a series of tests. Each test has two threads which are accessing shared variables. 

All shared variables are set to 0 before each test.

There are two broad groups of tests:
- Group 1: 
  - Each thread writes 1 to a shared variable (`x` for thread 1, and `y` for thread 2),
    and then reads from the other shared variable (`y` for thread 1, and `x` for thread 2)
  - Variants of the test are run with no barriers, with one barrier or atomic, or with two barriers or atomics
  - If out-of-order memory accesses occur, then the results of the reads of `x` and `y` can lead to both being 0.
- Group 2:
  - Thread 1 writes 2 to `x` and then 3 to `y`, while thread 2 reads from `y` and then reads from `x`
  - Variants of the test are run with no barriers, with one barrier or atomic, or with two barriers or atomics
  - If out-of-order memory accesses occur, then the results of the reads of `y` being 3, and of `x` being 0.


## Building and running the tests

### Requirements
* ARM64 or x86_64 based CPU
* C++ compiler that supports C++ 17 or (better) C++ 20
* (optional) CMake and ninja

By default, the Memory Model Tool will use the C++ 20 `std::binary_semaphore`. Unfortunately, this class is not
available in all compilers that say they support C++ 20. If necessary, define the `AVOID_CPP20_SEMAPHORE` macro to 
use the `basic_semaphore` class included in this project instead of `std::binary_semaphore`.

### Build

There are two options:

- Use the CMakeLists.txt file with CMake:
  ```
  mkdir build
  cd build
  cmake -G Ninja ../.
  ninja
  ```
- Use `g++` directly.

  For Ubuntu on Evergreen, first ensure that the correct C++ compiler is on the path:
  
  ```
  export "PATH=/opt/mongodbtoolchain/v4/bin:$PATH"
  ```

  On both Mac or Evergreen, compile using g++.
 
  - For C++ 20:
    ```
    g++ -o memory_model_test -O2 memory_model_test.cpp -lpthread -std=c++20 -Wall -Werror
    ```
  - For C++ 17 with the local `basic_semaphore`:
    ```
    g++ -o memory_model_test -O2 memory_model_test.cpp -lpthread -std=c++17 -Wall -Werror -DAVOID_CPP20_SEMAPHORE
    ```
  

Some tests use compiler barriers to prevent the compiler re-ordering memory accesses during optimisation.

Note: if you get compile errors related to `#include <semaphore>` or semaphores in general,
then check that you are using both the correct compiler and compiling for C++ 20, 
or use the `AVOID_CPP20_SEMAPHORE` macro to use the local `basic_semaphore`. 

The test uses the C++ semaphore library as it is supported on both Mac and Ubuntu.

### Running the test

`./memory_model_test` to run the default loop count of 1,000,000, using a single thread pair,
which typically takes a few tens of seconds.

`./memory_model_test -n <loop count>` to run a custom loop count of `<loop count>`.

`./memory_model_test -p <pair count>` to run a custom number of `<pair count>` thread pairs.
This invokes an experimental mode where more than one thread pair is executed simultaneously.
However, it appears that this option does not seem to increase the number of out-of-order events,
and is therefore not of great value. Note that the messages from each thread pair are currently interleaved.

### Expected results:

Each test displays if/when out of order operations are possible. 

Some tests should never show out of order operations because of either the correct use of memory barriers/atomics, 
or the processor design. If any of these tests report out of order operations, then that is an error.

Some tests **can** report out of order operations, but that does not mean they **will** report them.
This is due to the randomness of the timing between the two threads, as well as the low incidence of
measurable out-of-order effects on some test/hardware combinations.

All x86_64 and ARM64 processors will likely show some out of order operations for some of the first group of tests.
However, as consequence of the different memory models, only ARM can show out of order operations in the second group.

Current ARM64 results show that group two tests show high out-of-order rates on M1 Pro/Max processors,
but much lower (by several orders of magnitude) out-of-order rates on current Evergreen Graviton instances.

Here is an example of the output from running the tool on a Mac Studio with an M1 Max ARM processor.
Note that out-of-order operations are occurring in all scenarios where they are possible on an ARM64.


```
WiredTiger Memory Model Test
============================
Running on ARM64 with 1 thread pairs(s) and loop count 1000000

-- Group 1: Tests that have a read and a write in each thread --

Test name:        Test writes then reads
Test description: Each thread writes then reads. Out of orders ARE POSSIBLE.
Total of 176874 out of orders detected out of 1000000 iterations (17.6874%) in test Test writes then reads

Test name:        Test writes then reads with one barrier
Test description: Each thread writes then reads, with one barrier between the write and read on thread 2. Out of orders ARE POSSIBLE.
Total of 6898 out of orders detected out of 1000000 iterations (0.6898%) in test Test writes then reads with one barrier

Test name:        Test writes then reads with two barriers
Test description: Each thread writes then reads, with a barrier between the write and read on each thread. Out of orders are NOT POSSIBLE.
Total of 0 out of orders detected out of 1000000 iterations (0%) in test Test writes then reads with two barriers

Test name:        Test writes then reads with one atomic
Test description: Each thread writes then reads, with one atomic increment used for one write. Out of orders ARE POSSIBLE.
Total of 65025 out of orders detected out of 1000000 iterations (6.5025%) in test Test writes then reads with one atomic

Test name:        Test writes then reads with two atomics
Test description: Each thread writes then reads, with atomic increments used for both writes. Out of orders are NOT POSSIBLE.
Total of 0 out of orders detected out of 1000000 iterations (0%) in test Test writes then reads with two atomics

-- Group 2: Tests that have two reads in one thread, and two writes in the other thread --

Test name:        Test writes and reads
Test description: One thread has two writes, the other has two reads. Out of orders ARE POSSIBLE on ARM64.
Total of 9892 out of orders detected out of 1000000 iterations (0.9892%) in test Test writes and reads

Test name:        Test writes and reads, with barrier between writes
Test description: One thread has two writes with a barrier between them, the other has two reads. Out of orders ARE POSSIBLE on ARM64.
Total of 2671 out of orders detected out of 1000000 iterations (0.2671%) in test Test writes and reads, with barrier between writes

Test name:        Test writes and reads, with barrier between reads
Test description: One thread has two writes, the other has two reads with a barrier between them. Out of orders are NOT POSSIBLE.
Total of 0 out of orders detected out of 1000000 iterations (0%) in test Test writes and reads, with barrier between reads

Test name:        Test writes and reads, with barrier between writes and between reads
Test description: One thread has two writes with a barrier between them, the other has two reads with a barrier between them. Out of orders are NOT POSSIBLE.
Total of 0 out of orders detected out of 1000000 iterations (0%) in test Test writes and reads, with barrier between writes and between reads

Test name:        Test writes and reads, with atomics
Test description: One thread has two writes using atomic increments, the other has two reads. Out of orders are ARE POSSIBLE on ARM64.
Total of 810 out of orders detected out of 1000000 iterations (0.081%) in test Test writes and reads, with atomics

```

Here is an example of the output from running the tool on an ubuntu2004-arm64-small ARM64 Evergreen instance
(Evergreen has removed blank lines).

Note that out-of-order operations are occurring:
* much less frequently (by approx 3 orders of magnitude) compared to the example above from a Mac Studio with an M1 Max ARM processor.
* in all but one of scenarios where they are possible on an ARM64. 
  * It is unclear why the Group 2 test '_Test writes and reads, with barrier between writes_' is not showing any out-of-order 
    operations when they are possible on ARM64, but it could be they are too rare to show up.

```
WiredTiger Memory Model Test
============================
Running on ARM64 with 1 thread pairs(s) and loop count 100000000
-- Group 1: Tests that have a read and a write in each thread --
Test name:        Test writes then reads
Test description: Each thread writes then reads. Out of orders ARE POSSIBLE.
Total of 27147 out of orders detected out of 100000000 iterations (0.027147%) in test Test writes then reads
Test name:        Test writes then reads with one barrier
Test description: Each thread writes then reads, with one barrier between the write and read on thread 2. Out of orders ARE POSSIBLE.
Total of 359 out of orders detected out of 100000000 iterations (0.000359%) in test Test writes then reads with one barrier
Test name:        Test writes then reads with two barriers
Test description: Each thread writes then reads, with a barrier between the write and read on each thread. Out of orders are NOT POSSIBLE.
Total of 0 out of orders detected out of 100000000 iterations (0%) in test Test writes then reads with two barriers
Test name:        Test writes then reads with one atomic
Test description: Each thread writes then reads, with one atomic increment used for one write. Out of orders ARE POSSIBLE.
Total of 996 out of orders detected out of 100000000 iterations (0.000996%) in test Test writes then reads with one atomic
Test name:        Test writes then reads with two atomics
Test description: Each thread writes then reads, with atomic increments used for both writes. Out of orders are NOT POSSIBLE.
Total of 0 out of orders detected out of 100000000 iterations (0%) in test Test writes then reads with two atomics
-- Group 2: Tests that have two reads in one thread, and two writes in the other thread --
Test name:        Test writes and reads
Test description: One thread has two writes, the other has two reads. Out of orders ARE POSSIBLE on ARM64.
Total of 360 out of orders detected out of 100000000 iterations (0.00036%) in test Test writes and reads
Test name:        Test writes and reads, with barrier between writes
Test description: One thread has two writes with a barrier between them, the other has two reads. Out of orders ARE POSSIBLE on ARM64.
Total of 0 out of orders detected out of 100000000 iterations (0%) in test Test writes and reads, with barrier between writes
Test name:        Test writes and reads, with barrier between reads
Test description: One thread has two writes, the other has two reads with a barrier between them. Out of orders are NOT POSSIBLE.
Total of 0 out of orders detected out of 100000000 iterations (0%) in test Test writes and reads, with barrier between reads
Test name:        Test writes and reads, with barrier between writes and between reads
Test description: One thread has two writes with a barrier between them, the other has two reads with a barrier between them. Out of orders are NOT POSSIBLE.
Total of 0 out of orders detected out of 100000000 iterations (0%) in test Test writes and reads, with barrier between writes and between reads
Test name:        Test writes and reads, with atomics
Test description: One thread has two writes using atomic increments, the other has two reads. Out of orders are ARE POSSIBLE on ARM64.
Total of 41 out of orders detected out of 100000000 iterations (4.1e-05%) in test Test writes and reads, with atomics
```

# References

- This test was inspired by the work at https://preshing.com/20120515/memory-reordering-caught-in-the-act/
- [Memory barrier use example](https://developer.arm.com/documentation/den0042/a/Memory-Ordering/Memory-barriers/Memory-barrier-use-example) 