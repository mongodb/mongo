/*
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 *
 * See the README.md file for more information about this test and how to build and run it.
 *
 * This test was inspired by the work at
 *https://preshing.com/20120515/memory-reordering-caught-in-the-act/
 */

#include <iostream>
#include <list>
#include <thread>
#include <chrono>
#include <random>
#include <utility>
#include <unistd.h>

#ifdef AVOID_CPP20_SEMAPHORE
#include "basic_semaphore.h"
using binary_semaphore = basic_semaphore;
const std::string binary_semaphore_version = "basic_semaphore";
#else
#include <semaphore>
using std::binary_semaphore;
const std::string binary_semaphore_version = "std::binary_semaphore";
#endif

#if defined(x86_64) || defined(__x86_64__)
#define BARRIER_INSTRUCTION "mfence"
const bool is_arm64 = false;
#elif defined(__aarch64__)
#define BARRIER_INSTRUCTION "dmb ish"
const bool is_arm64 = true;
#endif

#define MEMORY_BARRIER asm volatile(BARRIER_INSTRUCTION ::: "memory")
#define COMPILER_BARRIER asm volatile("" ::: "memory")

template <typename code>
void
thread_function(std::string const &thread_name, binary_semaphore &start_semaphore,
  binary_semaphore &end_semaphore, int rng_seed, int loop_count, code code_param)
{
    std::mt19937 rng(rng_seed);
    for (int iterations = 0; iterations < loop_count; iterations++) {
        start_semaphore.acquire();
        while (rng() % 8 != 0) {
        }; // Short random delay
        code_param();
        end_semaphore.release();
    }
}

template <typename thread_1_code_t, typename thread_2_code_t, typename out_of_order_check_code_t>
class test_config {
public:
    test_config(std::string test_name, std::string test_description, thread_1_code_t thread_1_code,
      thread_2_code_t thread_2_code, out_of_order_check_code_t out_of_order_check_code,
      bool out_of_order_allowed)
        : _test_name(std::move(test_name)), _test_description(std::move(test_description)),
          _thread_1_code(thread_1_code), _thread_2_code(thread_2_code),
          _out_of_order_check_code(out_of_order_check_code),
          _out_of_order_allowed(out_of_order_allowed)
    {
    }

    std::string _test_name;
    std::string _test_description;
    thread_1_code_t _thread_1_code;
    thread_2_code_t _thread_2_code;
    out_of_order_check_code_t _out_of_order_check_code;
    bool _out_of_order_allowed;
};

template <typename thread_1_code, typename thread_2_code, typename out_of_order_check_code>
void
perform_test(test_config<thread_1_code, thread_2_code, out_of_order_check_code> config, int &x,
  int &y, int &r1, int &r2, binary_semaphore &start_semaphore1, binary_semaphore &start_semaphore2,
  binary_semaphore &end_semaphore1, binary_semaphore &end_semaphore2, std::ostream &ostream,
  int loop_count, bool progress)
{
    ostream << "Test name:        " << config._test_name << std::endl;
    ostream << "Test description: " << config._test_description << std::endl;

    std::thread thread_1([&]() {
        thread_function(
          "thread_one", start_semaphore1, end_semaphore1, 1, loop_count, config._thread_1_code);
    });
    std::thread thread_2([&]() {
        thread_function(
          "thread_two", start_semaphore2, end_semaphore2, 2, loop_count, config._thread_2_code);
    });

    int iterations = 0;
    int out_of_order_count = 0;

    for (iterations = 0; iterations < loop_count; iterations++) {
        x = 0;
        y = 0;
        r1 = 0;
        r2 = 0;

        // Release the start semaphores to allow the worker threads to start an iteration of their
        // work.
        start_semaphore1.release();
        start_semaphore2.release();
        // The threads do an iteration of their work at this point.
        // Wait on the end semaphores to know when they are finished.
        end_semaphore1.acquire();
        end_semaphore2.acquire();
        bool out_of_order = config._out_of_order_check_code();
        if (out_of_order) {
            out_of_order_count++;
            if (progress)
                ostream << out_of_order_count << " out of orders detected out of " << iterations
                        << " iterations ("
                        << 100.0f * double(out_of_order_count) / double(iterations) << "%)"
                        << std::endl;
        }

        if (progress && iterations % 1000 == 0 && iterations != 0) {
            ostream << '.' << std::flush;
            if (iterations % 50000 == 0) {
                ostream << std::endl;
            }
        }
    }

    if (progress)
        // Ensure we have a newline after the last '.' is printed
        ostream << std::endl;

    ostream << "Total of " << out_of_order_count << " out of orders detected out of " << iterations
            << " iterations (" << 100.0f * double(out_of_order_count) / double(iterations) << "%)"
            << " in test " << config._test_name << std::endl;
    if (!config._out_of_order_allowed && out_of_order_count > 0)
        ostream
          << "******** ERROR out of order operations were not allowed, but did occur. ********"
          << std::endl;
    ostream << std::endl;

    thread_1.join();
    thread_2.join();
}

void
thread_pair(int loop_count, std::ostream &ostream)
{

    binary_semaphore start_semaphore1{0};
    binary_semaphore start_semaphore2{0};
    binary_semaphore end_semaphore1{0};
    binary_semaphore end_semaphore2{0};

    // We declare the shared variables above the lambdas so the lambdas have access to them.

    /*
     * Spacers are inserted to push the fields x, y, r1 and r2 onto different cache lines. This
     * dramatically increases the chances of seeing out-of-order operations in group 2 tests on ARM
     * 64 Evergreen instances.
     */
    const int space = 128 - sizeof(int);
    struct var_holder {
        int x = 0;
        char spacer1[space];
        int y = 0;
        char spacer2[space];
        int r1 = 0;
        char spacer3[space];
        int r2 = 0;
    };

    var_holder vh;

    int &x = vh.x;
    int &y = vh.y;
    int &r1 = vh.r1;
    int &r2 = vh.r2;

    //////////////////////////////////////////////////
    // Code that has a read and a write in each thread.
    //////////////////////////////////////////////////
    auto thread_1_code_write_then_read = [&]() {
        x = 1;
        COMPILER_BARRIER;
        r1 = y;
    };
    auto thread_2_code_write_then_read = [&]() {
        y = 1;
        COMPILER_BARRIER;
        r2 = x;
    };

    auto thread_1_code_write_then_barrier_then_read = [&]() {
        x = 1;
        MEMORY_BARRIER;
        r1 = y;
    };
    auto thread_2_code_write_then_barrier_then_read = [&]() {
        y = 1;
        MEMORY_BARRIER;
        r2 = x;
    };

    auto thread_1_atomic_increment_and_read = [&]() {
        __atomic_add_fetch(&x, 1, __ATOMIC_SEQ_CST);
        r1 = y;
    };
    auto thread_2_atomic_increment_and_read = [&]() {
        __atomic_add_fetch(&y, 1, __ATOMIC_SEQ_CST);
        r2 = x;
    };

    auto out_of_order_check_code_for_write_then_read = [&]() { return r1 == 0 && r2 == 0; };

    /////////////////////////////////////////////////////////////////////////////
    // Code that has two reads in one thread, and two writes in the other thread.
    /////////////////////////////////////////////////////////////////////////////
    auto thread_1_code_write_then_write = [&]() {
        COMPILER_BARRIER;
        x = 2;
        COMPILER_BARRIER;
        y = 3;
        COMPILER_BARRIER;
    };
    auto thread_2_code_read_then_read = [&]() {
        COMPILER_BARRIER;
        r1 = y;
        COMPILER_BARRIER;
        r2 = x;
        COMPILER_BARRIER;
    };
    auto thread_1_code_two_atomic_increments = [&]() {
        __atomic_exchange_n(&x, 2, __ATOMIC_SEQ_CST);
        __atomic_exchange_n(&y, 3, __ATOMIC_SEQ_CST);
    };
    auto thread_1_code_write_then_barrier_then_write = [&]() {
        x = 2;
        MEMORY_BARRIER;
        y = 3;
    };
    auto thread_2_code_read_then_barrier_then_read = [&]() {
        r1 = y;
        MEMORY_BARRIER;
        r2 = x;
    };

    auto out_of_order_check_code_for_write_then_write = [&]() { return r1 == 3 && r2 == 0; };

    /////////////////////////////////////////////////////
    // Tests that have a read and a write in each thread.
    /////////////////////////////////////////////////////

    auto test_writes_then_reads = test_config("Test writes then reads",
      "Each thread writes then reads. Out of orders ARE POSSIBLE.", thread_1_code_write_then_read,
      thread_2_code_write_then_read, out_of_order_check_code_for_write_then_read, true);

    auto test_writes_then_reads_one_barrier = test_config("Test writes then reads with one barrier",
      "Each thread writes then reads, with one barrier between the write and read on thread 2. "
      "Out of orders ARE POSSIBLE.",
      thread_1_code_write_then_read, thread_2_code_write_then_barrier_then_read,
      out_of_order_check_code_for_write_then_read, true);

    auto test_writes_then_reads_two_barriers =
      test_config("Test writes then reads with two barriers",
        "Each thread writes then reads, with a barrier between the write and read on each thread. "
        "Out of orders are NOT POSSIBLE.",
        thread_1_code_write_then_barrier_then_read, thread_2_code_write_then_barrier_then_read,
        out_of_order_check_code_for_write_then_read, false);

    auto test_writes_then_reads_one_atomic = test_config("Test writes then reads with one atomic",
      "Each thread writes then reads, with one atomic increment used for one write. "
      "Out of orders ARE POSSIBLE.",
      thread_1_atomic_increment_and_read, thread_2_code_write_then_read,
      out_of_order_check_code_for_write_then_read, true);

    auto test_writes_then_reads_two_atomics = test_config("Test writes then reads with two atomics",
      "Each thread writes then reads, with atomic increments used for both writes. "
      "Out of orders are NOT POSSIBLE.",
      thread_1_atomic_increment_and_read, thread_2_atomic_increment_and_read,
      out_of_order_check_code_for_write_then_read, false);

    auto test_writes_then_reads_one_barrier_one_atomic =
      test_config("Test writes then reads with one barrier and one atomic",
        "Each thread writes then reads, with atomic increments used for one write, "
        "and a barrier used between the write and read in the other thread. "
        "Out of orders are NOT POSSIBLE.",
        thread_1_atomic_increment_and_read, thread_2_atomic_increment_and_read,
        out_of_order_check_code_for_write_then_read, false);

    ///////////////////////////////////////////////////////////////////////////////
    // Tests that have two reads in one thread, and two writes in the other thread.
    ///////////////////////////////////////////////////////////////////////////////

    auto test_writes_and_reads = test_config("Test writes and reads",
      "One thread has two writes, the other has two reads. "
      "Out of orders ARE POSSIBLE on ARM64.",
      thread_1_code_write_then_write, thread_2_code_read_then_read,
      out_of_order_check_code_for_write_then_write, is_arm64);

    auto test_writes_and_reads_barrier_between_writes =
      test_config("Test writes and reads, with barrier between writes",
        "One thread has two writes with a barrier between them, the other has two reads. "
        "Out of orders ARE POSSIBLE on ARM64.",
        thread_1_code_write_then_barrier_then_write, thread_2_code_read_then_read,
        out_of_order_check_code_for_write_then_write, is_arm64);

    auto test_writes_and_reads_barrier_between_reads =
      test_config("Test writes and reads, with barrier between reads",
        "One thread has two writes, the other has two reads with a barrier between them. "
        "Out of orders are NOT POSSIBLE.",
        thread_1_code_write_then_read, thread_2_code_read_then_barrier_then_read,
        out_of_order_check_code_for_write_then_write, false);

    auto test_writes_and_reads_barrier_between_writes_and_between_reads =
      test_config("Test writes and reads, with barrier between writes and between reads",
        "One thread has two writes with a barrier between them, "
        "the other has two reads with a barrier between them. "
        "Out of orders are NOT POSSIBLE.",
        thread_1_code_write_then_barrier_then_write, thread_2_code_read_then_barrier_then_read,
        out_of_order_check_code_for_write_then_write, false);

    auto test_writes_and_reads_atomics = test_config("Test writes and reads, with atomics",
      "One thread has two writes using atomic increments, the other has two reads. "
      "Out of orders are ARE POSSIBLE on ARM64.",
      thread_1_code_two_atomic_increments, thread_2_code_read_then_read,
      out_of_order_check_code_for_write_then_write, is_arm64);

    const bool progress = false;

    std::cout << "-- Group 1: Tests that have a read and a write in each thread --" << std::endl
              << std::endl;

    perform_test(test_writes_then_reads, x, y, r1, r2, start_semaphore1, start_semaphore2,
      end_semaphore1, end_semaphore2, std::cout, loop_count, progress);

    perform_test(test_writes_then_reads_one_barrier, x, y, r1, r2, start_semaphore1,
      start_semaphore2, end_semaphore1, end_semaphore2, std::cout, loop_count, progress);

    perform_test(test_writes_then_reads_two_barriers, x, y, r1, r2, start_semaphore1,
      start_semaphore2, end_semaphore1, end_semaphore2, std::cout, loop_count, progress);

    perform_test(test_writes_then_reads_one_atomic, x, y, r1, r2, start_semaphore1,
      start_semaphore2, end_semaphore1, end_semaphore2, std::cout, loop_count, progress);

    perform_test(test_writes_then_reads_two_atomics, x, y, r1, r2, start_semaphore1,
      start_semaphore2, end_semaphore1, end_semaphore2, std::cout, loop_count, progress);

    std::cout << "-- Group 2: Tests that have two reads in one thread, and two writes in the other "
                 "thread --"
              << std::endl
              << std::endl;

    perform_test(test_writes_and_reads, x, y, r1, r2, start_semaphore1, start_semaphore2,
      end_semaphore1, end_semaphore2, std::cout, loop_count, progress);

    perform_test(test_writes_and_reads_barrier_between_writes, x, y, r1, r2, start_semaphore1,
      start_semaphore2, end_semaphore1, end_semaphore2, std::cout, loop_count, progress);

    perform_test(test_writes_and_reads_barrier_between_reads, x, y, r1, r2, start_semaphore1,
      start_semaphore2, end_semaphore1, end_semaphore2, std::cout, loop_count, progress);

    perform_test(test_writes_and_reads_barrier_between_writes_and_between_reads, x, y, r1, r2,
      start_semaphore1, start_semaphore2, end_semaphore1, end_semaphore2, std::cout, loop_count,
      progress);

    perform_test(test_writes_and_reads_atomics, x, y, r1, r2, start_semaphore1, start_semaphore2,
      end_semaphore1, end_semaphore2, std::cout, loop_count, progress);
}

int
main(int argc, char *argv[])
{
    std::cout << "WiredTiger Memory Model Test" << std::endl;
    std::cout << "============================" << std::endl;

    int loop_count = 1000000;
    int num_thread_pairs = 1;

    int opt = 0;
    while ((opt = getopt(argc, argv, "n:p:")) != -1) {
        switch (opt) {
        case 'n':
            loop_count = atoi(optarg);
            break;
        case 'p':
            num_thread_pairs = atoi(optarg);
            break;
        case '?':
            std::cout << "Parameter error" << std::endl;
            break;
        default:
            break;
        }
    }

    std::cout << "C++ language standard: " << __cplusplus
              << ", with binary_semaphore: " << binary_semaphore_version << std::endl;

    if (is_arm64)
        std::cout << "Running on ARM64";
    else
        std::cout << "Running on x86";

    std::cout << " with " << num_thread_pairs << " thread pairs(s) and loop count " << loop_count
              << std::endl
              << std::endl;

    std::list<std::thread> threads;

    for (int tp = 0; tp < num_thread_pairs; tp++) {
        threads.emplace_back([&]() { thread_pair(loop_count, std::cout); });
    }

    for (auto &thread : threads) {
        thread.join();
    }
}
