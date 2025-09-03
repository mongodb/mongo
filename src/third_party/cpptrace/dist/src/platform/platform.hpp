#ifndef PLATFORM_HPP
#define PLATFORM_HPP

#define IS_WINDOWS 0
#define IS_LINUX 0
#define IS_APPLE 0

#if defined(_WIN32)
 #undef IS_WINDOWS
 #define IS_WINDOWS 1
#elif defined(__linux)
 #undef IS_LINUX
 #define IS_LINUX 1
#elif defined(__APPLE__)
 #undef IS_APPLE
 #define IS_APPLE 1
#else
 #error "Unexpected platform"
#endif

#define IS_CLANG 0
#define IS_GCC 0
#define IS_MSVC 0

#if defined(__clang__)
 #undef IS_CLANG
 #define IS_CLANG 1
#elif defined(__GNUC__) || defined(__GNUG__)
 #undef IS_GCC
 #define IS_GCC 1
#elif defined(_MSC_VER)
 #undef IS_MSVC
 #define IS_MSVC 1
#else
 #error "Unsupported compiler"
#endif

#define IS_LIBSTDCXX 0
#define IS_LIBCXX 0
#if defined(__GLIBCXX__) || defined(__GLIBCPP__)
#undef IS_LIBSTDCXX
#define IS_LIBSTDCXX 1
#elif defined(_LIBCPP_VERSION)
#undef IS_LIBCXX
#define IS_LIBCXX 1
#endif

#endif
