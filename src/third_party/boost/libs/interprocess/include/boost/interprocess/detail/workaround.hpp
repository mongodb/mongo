//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2015. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/interprocess for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTERPROCESS_DETAIL_WORKAROUND_HPP
#define BOOST_INTERPROCESS_DETAIL_WORKAROUND_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif
#
#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#if defined(BOOST_INTERPROCESS_FORCE_NATIVE_EMULATION) && defined(BOOST_INTERPROCESS_FORCE_GENERIC_EMULATION) 
#error "BOOST_INTERPROCESS_FORCE_NATIVE_EMULATION && BOOST_INTERPROCESS_FORCE_GENERIC_EMULATION can't be defined at the same time"
#endif

//#define BOOST_INTERPROCESS_FORCE_NATIVE_EMULATION

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
   #define BOOST_INTERPROCESS_WINDOWS
   #if !defined(BOOST_INTERPROCESS_FORCE_NATIVE_EMULATION) && !defined(BOOST_INTERPROCESS_FORCE_GENERIC_EMULATION)
      #define BOOST_INTERPROCESS_FORCE_GENERIC_EMULATION
   #endif
   #define BOOST_INTERPROCESS_HAS_KERNEL_BOOTTIME
#else
   #include <unistd.h>

   #if defined (__CYGWIN__) && (!defined(_POSIX_C_SOURCE) && (_POSIX_C_SOURCE < 200112L))
   #error "Error: Compiling on Cygwin without POSIX is not supported. Please define _XOPEN_SOURCE >= 600 or _POSIX_C_SOURCE >= 200112 when compiling"
   #endif

   //////////////////////////////////////////////////////
   //Check for XSI shared memory objects. They are available in nearly all UNIX platforms
   //////////////////////////////////////////////////////
   #if !defined(__QNXNTO__) && !defined(__ANDROID__) && !defined(__HAIKU__) && !(__VXWORKS__) && !(__EMSCRIPTEN__)
      #define BOOST_INTERPROCESS_XSI_SHARED_MEMORY_OBJECTS
   #endif

   //////////////////////////////////////////////////////
   // From SUSv3/UNIX 98, pthread_mutexattr_settype is mandatory
   //////////////////////////////////////////////////////
   #if defined(_XOPEN_UNIX) && ((_XOPEN_VERSION + 0) >= 500)
      #define BOOST_INTERPROCESS_POSIX_RECURSIVE_MUTEXES
   #endif

   //////////////////////////////////////////////////////
   // _POSIX_THREAD_PROCESS_SHARED (POSIX.1b/POSIX.4)
   //////////////////////////////////////////////////////
   #if defined(_POSIX_THREAD_PROCESS_SHARED) && ((_POSIX_THREAD_PROCESS_SHARED + 0) > 0)
      //Cygwin defines _POSIX_THREAD_PROCESS_SHARED but does not implement it.
      #if defined(__CYGWIN__)
         #define BOOST_INTERPROCESS_BUGGY_POSIX_PROCESS_SHARED
      #elif defined(__APPLE__)
         //The pthreads implementation of darwin stores a pointer to a mutex inside the condition
         //structure so real sharing between processes is broken. See:
         //https://opensource.apple.com/source/libpthread/libpthread-301.30.1/src/pthread_cond.c.auto.html
         //in method pthread_cond_wait
         #define BOOST_INTERPROCESS_BUGGY_POSIX_PROCESS_SHARED
      #endif

      //If buggy _POSIX_THREAD_PROCESS_SHARED is detected avoid using it
      #if defined(BOOST_INTERPROCESS_BUGGY_POSIX_PROCESS_SHARED)
         #undef BOOST_INTERPROCESS_BUGGY_POSIX_PROCESS_SHARED
      #else
         #define BOOST_INTERPROCESS_POSIX_PROCESS_SHARED
      #endif
   #endif

   //////////////////////////////////////////////////////
   //    BOOST_INTERPROCESS_POSIX_ROBUST_MUTEXES
   //////////////////////////////////////////////////////
   #if (_XOPEN_SOURCE >= 700 || _POSIX_C_SOURCE >= 200809L)
      #define BOOST_INTERPROCESS_POSIX_ROBUST_MUTEXES
   #endif

   //////////////////////////////////////////////////////
   // _POSIX_SHARED_MEMORY_OBJECTS (POSIX.1b/POSIX.4)
   //////////////////////////////////////////////////////
   #if ( defined(_POSIX_SHARED_MEMORY_OBJECTS) && ((_POSIX_SHARED_MEMORY_OBJECTS + 0) > 0) ) ||\
         (defined(__vms) && __CRTL_VER >= 70200000)
      #define BOOST_INTERPROCESS_POSIX_SHARED_MEMORY_OBJECTS
      //Some systems have filesystem-based resources, so the
      //portable "/shmname" format does not work due to permission issues
      //For those systems we need to form a path to a temporary directory:
      //          hp-ux               tru64               vms               freebsd
      #if defined(__hpux) || defined(__osf__) || defined(__vms) || (defined(__FreeBSD__) && (__FreeBSD__ < 7))
         #define BOOST_INTERPROCESS_FILESYSTEM_BASED_POSIX_SHARED_MEMORY
      //Some systems have "jailed" environments where shm usage is restricted at runtime
      //and temporary file based shm is possible in those executions.
      #elif defined(__FreeBSD__)
         #define BOOST_INTERPROCESS_RUNTIME_FILESYSTEM_BASED_POSIX_SHARED_MEMORY
      #endif
   #endif

   //////////////////////////////////////////////////////
   // _POSIX_MAPPED_FILES (POSIX.1b/POSIX.4)
   //////////////////////////////////////////////////////
   #if defined(_POSIX_MAPPED_FILES) && ((_POSIX_MAPPED_FILES + 0) > 0)
      #define BOOST_INTERPROCESS_POSIX_MAPPED_FILES
   #endif

   //////////////////////////////////////////////////////
   // _POSIX_SEMAPHORES (POSIX.1b/POSIX.4)
   //////////////////////////////////////////////////////
   #if ( defined(_POSIX_SEMAPHORES) && ((_POSIX_SEMAPHORES + 0) > 0) ) ||\
       ( defined(__FreeBSD__) && (__FreeBSD__ >= 4)) || \
         defined(__APPLE__)
      #define BOOST_INTERPROCESS_POSIX_NAMED_SEMAPHORES
      //MacOsX declares _POSIX_SEMAPHORES but sem_init returns ENOSYS
      #if !defined(__APPLE__)
         #define BOOST_INTERPROCESS_POSIX_UNNAMED_SEMAPHORES
      #endif
      #if defined(__osf__) || defined(__vms)
         #define BOOST_INTERPROCESS_FILESYSTEM_BASED_POSIX_SEMAPHORES
      #endif
   #endif

   //////////////////////////////////////////////////////
   // _POSIX_BARRIERS (SUSv3/Unix03)
   //////////////////////////////////////////////////////
   #if defined(_POSIX_BARRIERS) && ((_POSIX_BARRIERS + 0) >= 200112L)
      #define BOOST_INTERPROCESS_POSIX_BARRIERS
   #endif

   //////////////////////////////////////////////////////
   // _POSIX_TIMEOUTS (SUSv3/Unix03)
   //////////////////////////////////////////////////////
   #if defined(_POSIX_TIMEOUTS) && ((_POSIX_TIMEOUTS + 0L) >= 200112L)
      #define BOOST_INTERPROCESS_POSIX_TIMEOUTS
   #endif

   //////////////////////////////////////////////////////
   // Detect BSD derivatives to detect sysctl
   //////////////////////////////////////////////////////
   #if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__APPLE__)
      #define BOOST_INTERPROCESS_BSD_DERIVATIVE
      //Some *BSD systems (OpenBSD & NetBSD) need sys/param.h before sys/sysctl.h, whereas
      //others (FreeBSD & Darwin) need sys/types.h
      #include <sys/types.h>
      #include <sys/param.h>
      #include <sys/sysctl.h>
      #if defined(CTL_KERN) && defined (KERN_BOOTTIME)
         //#define BOOST_INTERPROCESS_HAS_KERNEL_BOOTTIME
      #endif
   #endif

   //////////////////////////////////////////////////////
   //64 bit offset
   //////////////////////////////////////////////////////
   #if (defined (_V6_ILP32_OFFBIG)  &&(_V6_ILP32_OFFBIG   - 0 > 0)) ||\
       (defined (_V6_LP64_OFF64)    &&(_V6_LP64_OFF64     - 0 > 0)) ||\
       (defined (_V6_LPBIG_OFFBIG)  &&(_V6_LPBIG_OFFBIG   - 0 > 0)) ||\
       (defined (_XBS5_ILP32_OFFBIG)&&(_XBS5_ILP32_OFFBIG - 0 > 0)) ||\
       (defined (_XBS5_LP64_OFF64)  &&(_XBS5_LP64_OFF64   - 0 > 0)) ||\
       (defined (_XBS5_LPBIG_OFFBIG)&&(_XBS5_LPBIG_OFFBIG - 0 > 0)) ||\
       (defined (_FILE_OFFSET_BITS) &&(_FILE_OFFSET_BITS  - 0 >= 64))||\
       (defined (_FILE_OFFSET_BITS) &&(_FILE_OFFSET_BITS  - 0 >= 64))
      #define BOOST_INTERPROCESS_UNIX_64_BIT_OR_BIGGER_OFF_T
   #endif

   //////////////////////////////////////////////////////
   //posix_fallocate
   //////////////////////////////////////////////////////
   #if (_XOPEN_SOURCE >= 600 || _POSIX_C_SOURCE >= 200112L)
   #define BOOST_INTERPROCESS_POSIX_FALLOCATE
   #endif

#endif   //!defined(BOOST_INTERPROCESS_WINDOWS)

#if defined(BOOST_INTERPROCESS_WINDOWS) || defined(BOOST_INTERPROCESS_POSIX_MAPPED_FILES)
#  define BOOST_INTERPROCESS_MAPPED_FILES
#endif

//Now declare some Boost.Interprocess features depending on the implementation
#if defined(BOOST_INTERPROCESS_POSIX_NAMED_SEMAPHORES) && !defined(BOOST_INTERPROCESS_POSIX_SEMAPHORES_NO_UNLINK)
   #define BOOST_INTERPROCESS_NAMED_MUTEX_USES_POSIX_SEMAPHORES
   #define BOOST_INTERPROCESS_NAMED_SEMAPHORE_USES_POSIX_SEMAPHORES
#endif

#if    !defined(BOOST_NO_CXX11_RVALUE_REFERENCES) && !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
   #define BOOST_INTERPROCESS_PERFECT_FORWARDING
#endif

// Timeout duration use if BOOST_INTERPROCESS_ENABLE_TIMEOUT_WHEN_LOCKING is set
#ifndef BOOST_INTERPROCESS_TIMEOUT_WHEN_LOCKING_DURATION_MS
   #define BOOST_INTERPROCESS_TIMEOUT_WHEN_LOCKING_DURATION_MS 10000
#endif


// Max open or create tries with managed memory segments
#ifndef BOOST_INTERPROCESS_MANAGED_OPEN_OR_CREATE_INITIALIZE_MAX_TRIES
   #define BOOST_INTERPROCESS_MANAGED_OPEN_OR_CREATE_INITIALIZE_MAX_TRIES 20u
#endif

// Maximum timeout in seconds with open or create tries with managed memory segments
// waiting the creator to initialize the shared memory
#ifndef BOOST_INTERPROCESS_MANAGED_OPEN_OR_CREATE_INITIALIZE_TIMEOUT_SEC
   #define BOOST_INTERPROCESS_MANAGED_OPEN_OR_CREATE_INITIALIZE_TIMEOUT_SEC 300u
#endif

//Other switches
//BOOST_INTERPROCESS_MSG_QUEUE_USES_CIRC_INDEX
//message queue uses a circular queue as index instead of an array (better performance)
//Boost version < 1.52 uses an array, so undef this if you want to communicate
//with processes compiled with those versions.
#define BOOST_INTERPROCESS_MSG_QUEUE_CIRCULAR_INDEX

//Macros for documentation purposes. For code, expands to the argument
#define BOOST_INTERPROCESS_IMPDEF(TYPE) TYPE
#define BOOST_INTERPROCESS_SEEDOC(TYPE) TYPE
#define BOOST_INTERPROCESS_DOC1ST(TYPE1, TYPE2) TYPE2
#define BOOST_INTERPROCESS_I ,
#define BOOST_INTERPROCESS_DOCIGN(T1) T1

//#define BOOST_INTERPROCESS_DISABLE_FORCEINLINE

#if defined(BOOST_INTERPROCESS_DISABLE_FORCEINLINE)
   #define BOOST_INTERPROCESS_FORCEINLINE inline
#elif defined(BOOST_INTERPROCESS_FORCEINLINE_IS_BOOST_FORCELINE)
   #define BOOST_INTERPROCESS_FORCEINLINE BOOST_FORCEINLINE
#elif defined(BOOST_MSVC) && (_MSC_VER < 1900 || defined(_DEBUG))
   //"__forceinline" and MSVC seems to have some bugs in old versions and in debug mode
   #define BOOST_INTERPROCESS_FORCEINLINE inline
#elif defined(BOOST_CLANG) || (defined(BOOST_GCC) && ((__GNUC__ <= 5) || defined(__MINGW32__)))
   //Older GCCs have problems with forceinline
   //Clang can have code bloat issues with forceinline, see
   //https://lists.boost.org/boost-users/2023/04/91445.php and
   //https://github.com/llvm/llvm-project/issues/62202
   #define BOOST_INTERPROCESS_FORCEINLINE inline
#else
   #define BOOST_INTERPROCESS_FORCEINLINE BOOST_FORCEINLINE
#endif

#ifdef BOOST_WINDOWS

#define BOOST_INTERPROCESS_WCHAR_NAMED_RESOURCES

#ifdef __clang__
   #define BOOST_INTERPROCESS_DISABLE_DEPRECATED_WARNING _Pragma("clang diagnostic push") \
                                                         _Pragma("clang diagnostic ignored \"-Wdeprecated-declarations\"")
   #define BOOST_INTERPROCESS_RESTORE_WARNING            _Pragma("clang diagnostic pop")
#else // __clang__
   #define BOOST_INTERPROCESS_DISABLE_DEPRECATED_WARNING __pragma(warning(push)) \
                                                         __pragma(warning(disable : 4996))
   #define BOOST_INTERPROCESS_RESTORE_WARNING            __pragma(warning(pop))
#endif // __clang__

#endif

#if defined(BOOST_HAS_THREADS) 
#  if defined(_MSC_VER) || defined(__MWERKS__) || defined(__MINGW32__) ||  defined(__BORLANDC__)
     //no reentrant posix functions (eg: localtime_r)
#  elif (!defined(__hpux) || (defined(__hpux) && defined(_REENTRANT)))
#   define BOOST_INTERPROCESS_HAS_REENTRANT_STD_FUNCTIONS
#  endif
#endif

namespace boost {
namespace interprocess {

template <typename T1>
BOOST_FORCEINLINE BOOST_CXX14_CONSTEXPR void ignore(T1 const&)
{}

}} //namespace boost::interprocess {

#if !(defined BOOST_NO_EXCEPTIONS)
#    define BOOST_INTERPROCESS_TRY { try
#    define BOOST_INTERPROCESS_CATCH(x) catch(x)
#    define BOOST_INTERPROCESS_RETHROW throw;
#    define BOOST_INTERPROCESS_CATCH_END }
#else
#    if !defined(BOOST_MSVC) || BOOST_MSVC >= 1900
#        define BOOST_INTERPROCESS_TRY { if (true)
#        define BOOST_INTERPROCESS_CATCH(x) else if (false)
#    else
// warning C4127: conditional expression is constant
#        define BOOST_INTERPROCESS_TRY { \
             __pragma(warning(push)) \
             __pragma(warning(disable: 4127)) \
             if (true) \
             __pragma(warning(pop))
#        define BOOST_INTERPROCESS_CATCH(x) else \
             __pragma(warning(push)) \
             __pragma(warning(disable: 4127)) \
             if (false) \
             __pragma(warning(pop))
#    endif
#    define BOOST_INTERPROCESS_RETHROW
#    define BOOST_INTERPROCESS_CATCH_END }
#endif

#ifndef BOOST_NO_CXX11_STATIC_ASSERT
#  ifndef BOOST_NO_CXX11_VARIADIC_MACROS
#     define BOOST_INTERPROCESS_STATIC_ASSERT( ... ) static_assert(__VA_ARGS__, #__VA_ARGS__)
#  else
#     define BOOST_INTERPROCESS_STATIC_ASSERT( B ) static_assert(B, #B)
#  endif
#else
namespace boost {
   namespace interprocess {
      namespace dtl {

         template<bool B>
         struct STATIC_ASSERTION_FAILURE;

         template<>
         struct STATIC_ASSERTION_FAILURE<true> {};

         template<unsigned> struct static_assert_test {};

      }
   }
}

#define BOOST_INTERPROCESS_STATIC_ASSERT(B) \
         typedef ::boost::interprocess::dtl::static_assert_test<\
            (unsigned)sizeof(::boost::interprocess::dtl::STATIC_ASSERTION_FAILURE<bool(B)>)>\
               BOOST_JOIN(boost_container_static_assert_typedef_, __LINE__) BOOST_ATTRIBUTE_UNUSED

#endif

#ifndef BOOST_NO_CXX11_STATIC_ASSERT
#  ifndef BOOST_NO_CXX11_VARIADIC_MACROS
#     define BOOST_INTERPROCESS_STATIC_ASSERT_MSG( ... ) static_assert(__VA_ARGS__)
#  else
#     define BOOST_INTERPROCESS_STATIC_ASSERT_MSG( B, Msg ) static_assert( B, Msg )
#  endif
#else
#     define BOOST_INTERPROCESS_STATIC_ASSERT_MSG( B, Msg ) BOOST_INTERPROCESS_STATIC_ASSERT( B )
#endif

#if !defined(BOOST_NO_CXX17_INLINE_VARIABLES)
#  define BOOST_INTERPROCESS_CONSTANT_VAR BOOST_INLINE_CONSTEXPR
#else
#  define BOOST_INTERPROCESS_CONSTANT_VAR static BOOST_CONSTEXPR_OR_CONST
#endif

#if defined(__GNUC__) && ((__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__) >= 40600)
#define BOOST_INTERPROCESS_GCC_COMPATIBLE_HAS_DIAGNOSTIC_IGNORED
#elif defined(__clang__)
#define BOOST_INTERPROCESS_GCC_COMPATIBLE_HAS_DIAGNOSTIC_IGNORED
#endif


////////////////////////////////////////////
//
//    BOOST_INTERPROCESS_EINTR_RETRY
//
////////////////////////////////////////////

//#define DISABLE_BOOST_INTERPROCESS_EINTR_RETRY
#if !defined(DISABLE_BOOST_INTERPROCESS_EINTR_RETRY) && defined(__GNUC__)

/* taken from glibc unistd.h and fixes musl */
#define BOOST_INTERPROCESS_EINTR_RETRY(RESULTTYPE, FAILUREVALUE, EXPRESSION) \
  (__extension__                                   \
    ({ RESULTTYPE __result;                        \
       do __result = (RESULTTYPE) (EXPRESSION);    \
       while (__result == FAILUREVALUE && errno == EINTR);  \
       __result; }))

#else    //!defined(DISABLE_BOOST_INTERPROCESS_EINTR_RETRY) && defined(__GNUC__)

#define BOOST_INTERPROCESS_EINTR_RETRY(RESULTTYPE, FAILUREVALUE, EXPRESSION) ((RESULTTYPE)(EXPRESSION))

#endif   //!defined(DISABLE_BOOST_INTERPROCESS_EINTR_RETRY) && defined(__GNUC__)

#endif   //#ifndef BOOST_INTERPROCESS_DETAIL_WORKAROUND_HPP
