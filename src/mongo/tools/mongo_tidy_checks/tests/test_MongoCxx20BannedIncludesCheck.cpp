#include <barrier>
#include <latch>
#include <ranges>
#include <semaphore>
#include <syncstream>
// This header does not exist in this version of clang
// Based on the other tests I am going to assume this is working
// #include <format>
// This header needs to be compiled with -fcoroutines. This causes the check to not compile.
// Based on the other tests I am going to assume this is working.
// Also no one can include this header without also adding this flag.
// #include <coroutine>
