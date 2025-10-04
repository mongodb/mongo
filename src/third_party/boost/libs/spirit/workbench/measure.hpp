// Copyright David Abrahams, Matthias Troyer, Michael Gauckler
// 2005. Distributed under the Boost Software License, Version
// 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#if !defined(BOOST_SPIRIT_TEST_BENCHMARK_HPP)
#define BOOST_SPIRIT_TEST_BENCHMARK_HPP

#ifdef _MSC_VER
// inline aggressively
# pragma inline_recursion(on) // turn on inline recursion
# pragma inline_depth(255)    // max inline depth
# define _SECURE_SCL 0 
#endif

#include "high_resolution_timer.hpp"
#include <iostream>
#include <cstring>
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/stringize.hpp>

namespace test
{
    // This value is required to ensure that a smart compiler's dead
    // code elimination doesn't optimize away anything we're testing.
    // We'll use it to compute the return code of the executable to make
    // sure it's needed.
    int live_code;

    // Call objects of the given Accumulator type repeatedly
    template <class Accumulator>
    void hammer(long const repeats)
    {
        // Strategy: because the sum in an accumulator after each call
        // depends on the previous value of the sum, the CPU's pipeline
        // might be stalled while waiting for the previous addition to
        // complete.  Therefore, we allocate an array of accumulators,
        // and update them in sequence, so that there's no dependency
        // between adjacent addition operations.
        //
        // Additionally, if there were only one accumulator, the
        // compiler or CPU might decide to update the value in a
        // register rather that writing it back to memory.  we want each
        // operation to at least update the L1 cache.  *** Note: This
        // concern is specific to the particular application at which
        // we're targeting the test. ***

        // This has to be at least as large as the number of
        // simultaneous accumulations that can be executing in the
        // compiler pipeline.  A safe number here is larger than the
        // machine's maximum pipeline depth. If you want to test the L2
        // or L3 cache, or main memory, you can increase the size of
        // this array.  1024 is an upper limit on the pipeline depth of
        // current vector machines.
        
        const std::size_t number_of_accumulators = 1024;
        live_code = 0; // reset to zero

        Accumulator a[number_of_accumulators];
        
        for (long iteration = 0; iteration < repeats; ++iteration)
        {
            for (Accumulator* ap = a;  ap < a + number_of_accumulators; ++ap)
            {
                ap->benchmark();
            }
        }

        // Accumulate all the partial sums to avoid dead code
        // elimination.
        for (Accumulator* ap = a; ap < a + number_of_accumulators; ++ap)
        {
            live_code += ap->val;
        }
    }

    // Measure the time required to hammer accumulators of the given type
    template <class Accumulator>
    double measure(long const repeats)
    {
        // Hammer accumulators a couple of times to ensure the
        // instruction cache is full of our test code, and that we don't
        // measure the cost of a page fault for accessing the data page
        // containing the memory where the accumulators will be
        // allocated
        hammer<Accumulator>(repeats);
        hammer<Accumulator>(repeats);

        // Now start a timer
        util::high_resolution_timer time;
        hammer<Accumulator>(repeats);   // This time, we'll measure
        return time.elapsed();          // return the elapsed time
    }
    
    template <class Accumulator>
    void report(char const* name, long const repeats)
    {
        std::cout.precision(10);
        std::cout << name << ": ";
        for (int i = 0; i < (20-int(strlen(name))); ++i)
            std::cout << ' ';
        std::cout << std::fixed << test::measure<Accumulator>(repeats) << " [s] ";
        Accumulator acc; 
        acc.benchmark(); 
        std::cout << std::hex << "{checksum: " << acc.val << "}";
        std::cout << std::flush << std::endl;
    }
    
    struct base
    {
        base() : val(0) {}
        int val;    // This is needed to avoid dead-code elimination
    };
    
#define BOOST_SPIRIT_TEST_HAMMER(r, data, elem)                     \
    test::hammer<elem>(repeats);
    /***/

#define BOOST_SPIRIT_TEST_MEASURE(r, data, elem)                    \
    test::report<elem>(BOOST_PP_STRINGIZE(elem), repeats);          \
    /***/

#define BOOST_SPIRIT_TEST_BENCHMARK(max_repeats, FSeq)              \
    long repeats = 100;                                             \
    double measured = 0;                                            \
    while (measured < 2.0 && repeats <= max_repeats)                \
    {                                                               \
        repeats *= 10;                                              \
        util::high_resolution_timer time;                           \
        BOOST_PP_SEQ_FOR_EACH(BOOST_SPIRIT_TEST_HAMMER, _, FSeq)    \
        measured = time.elapsed();                                  \
    }                                                               \
    BOOST_PP_SEQ_FOR_EACH(BOOST_SPIRIT_TEST_MEASURE, _, FSeq)       \
    /***/
}

#endif
