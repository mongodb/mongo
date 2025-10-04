# TSAN playground

This is a playground for testing TSAN (Thread Sanitizer) in a controlled environment to better understand its behavior and limitations.

Currently, the main tested limitation is TSAN's ability to detect synchronization through standalone barriers.

The `tsan_playground.c` file contains a simple example of N threads (4 by default) that read and modify a shared buffer in a defined order. The threads use an abstract lock-free acquire-release API to synchronize their access to the shared resource.

All the headers contain different implementations of the acquire-release API, which are used to test TSAN's behavior with different approaches and implementations.


## How to build
```
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchains/mongodbtoolchain_stable_clang.cmake -DCMAKE_BUILD_TYPE=TSan ..
cmake --build . -j 16
```

## How to run
Every implementation can be tested by running a separate binary.

You can also run the `collect_warnings.sh` script to see all the warnings for different synchronization implementations.

## Results
The tests were performed on both x86 and ARM architectures and on both clang and gcc compilers. However, the mongodbtoolchain v5 gcc compiler does not support TSAN, so GCC was taken from the system (Ubuntu 24.04).

The conclusions made from running the test with different implementations are as follows:
- TSAN works correctly only with GCC or C11 atomic-based acquire-release API.
- TSAN does not detect synchronization through standalone barriers, which is a limitation of the tool.
    - Using standalone fences with TSAN even causes a compilation error in GCC: `'atomic_thread_fence' is not supported with '-fsanitize=thread'`
    - All standalone barrier implementations (WT, C11, and GCC) generate the same number of false-positive warnings.
- There is no difference between running the test on x86 and ARM architectures, as TSAN behaves the same way on both platforms.
- On ARM, the clang compiler generates more warnings than GCC.
