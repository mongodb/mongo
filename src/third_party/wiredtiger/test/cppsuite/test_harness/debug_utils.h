#ifndef DEBUG_UTILS_H
#define DEBUG_UTILS_H

#include <cstdint>
#include <iostream>
#include <string>

/* Define helpful functions related to debugging. */
namespace test_harness {

#define DEBUG_ABORT -1
#define DEBUG_ERROR 0
#define DEBUG_INFO 1

static int64_t _trace_level = 0;

/* Used to print out traces for debugging purpose. */
static void
debug_info(const std::string &str, int64_t trace_threshold, int64_t trace_level)
{
    if (trace_threshold >= trace_level)
        std::cout << str << std::endl;
}

} // namespace test_harness

#endif
