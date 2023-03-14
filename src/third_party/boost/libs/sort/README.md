<h1>BOOST SORT</H1>

<H2>Introduction</h2>

The goal of the Boost Sort Library is provide to the users, the most modern and fast sorting algorithms.

This library provides stable and not stable sorting algorithms, in single thread and parallel versions.

These algorithms do not use any other library or utility. The parallel algorithms need a C++11 compliant compiler.

<h2>Single Thread Algorithms</h2>


                      |       |                            |                               |                     |
    Algorithm         |Stable |   Additional memory        |Best, average, and worst case  | Comparison method   |
    ------------------|-------|----------------------------|-------------------------------|---------------------|
    spreadsort        |  no   |      key_length            | N, N sqrt(LogN),              | Hybrid radix sort   |
                      |       |                            | min(N logN, N key_length)     |                     |
    pdqsort           |  no   |      Log N                 | N, N LogN, N LogN             | Comparison operator |
    spinsort          |  yes  |      N / 2                 | N, N LogN, N LogN             | Comparison operator |
    flat_stable_sort  |  yes  |size of the data / 256 + 8K | N, N LogN, N LogN             | Comparison operator |
                      |       |                            |                               |                     |


- **spreadsort** is a novel hybrid radix sort algorithm, extremely fast, designed and developed by Steven Ross.

- **pdqsort** is a improvement of the quick sort algorithm, designed and developed by Orson Peters.

- **spinsort** is a stable sort, fast with random and with near sorted data, designed and developed by Francisco Tapia.

- **flat_stable_sort** stable sort with a small additional memory (around 1% of the size of the data), provide the 80% - 90% of the speed of spinsort, being fast with random and with near sorted data, designed and developed by Francisco Tapia.


<h2>Parallel Algorithms</h2>


                          |       |                        |                              |
    Algorithm             |Stable |   Additional memory    |Best, average, and worst case |
    ----------------------|-------|------------------------|------------------------------|
    block_indirect_sort   |  no   |block_size * num_threads| N, N LogN , N LogN           |
    sample_sort           |  yes  |        N               | N, N LogN , N LogN           |
    parallel_stable_sort  |  yes  |      N / 2             | N, N LogN , N LogN           |
                          |       |                        |                              |


- **Sample_sort** is a implementation of the [Samplesort algorithm](https://en.wikipedia.org/wiki/Samplesort)  done by Francisco Tapia.

- **Parallel_stable_sort** is based on the samplesort algorithm, but using a half of the memory used by sample_sort, conceived and implemented by Francisco Tapia.

- **Block_indirect_sort** is a novel parallel sort algorithm, very fast, with low additional memory consumption, conceived and implemented by Francisco Tapia.

The **block_size** is an internal parameter of the algorithm, which in order to achieve the
highest speed, changes according to the size of the objects to sort according to the next table. The strings use a block_size of 128.


                                    |        |         |         |         |         |         |          |
    object size (bytes)             | 1 - 15 | 16 - 31 | 32 - 63 | 64 - 127|128 - 255|256 - 511| 512 -    |
    --------------------------------|--------|---------|---------|---------|---------|---------|----------|
    block_size (number of elements) |  4096  |  2048   |   1024  |   768   |   512   |   256   |  128     |
                                    |        |         |         |         |         |         |          |


<h2>Installation </h2>
- This library is **include only**.
- Don't need to link with any static or dynamic library. Only need a C++11 compiler
- Only need to include the file boost/sort/sort.hpp


<h2>Author and Copyright</h2>
This library is integrated in the [Boost Library](https://boost.org) .


Copyright 2017

- [Steven Ross *(spreadsort@gmail.com)* ](mail:spreadsort@gmail.com)
- [Francisco Tapia *(fjtapia@gmail.com)* ](mail:fjtapia@gmail.com)
- [Orson Peters *(orsonpeters@gmail.com)* ](mail:orsonpeters@gmail.com)

Distributed under the [Boost Software License, Version 1.0. ](http://www.boost.org/LICENSE_1_0.txt)  (See http://www.boost.org/LICENSE_1_0.txt)
