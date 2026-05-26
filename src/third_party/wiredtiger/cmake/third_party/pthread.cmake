# pthread (POSIX threading): capability detection.
#
# Uses CMake's standard Threads module, which selects pthread on POSIX and
# Win32 threading on Windows. Consumers link the Threads::Threads imported
# target.
#
# Layer 1 (capability): HAVE_LIBPTHREAD — true when threading is available.
# pthread is non-optional on supported platforms; configure fails without it.

if(TARGET Threads::Threads)
    # Avoid redefining the imported library.
    return()
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
set(HAVE_LIBPTHREAD ${Threads_FOUND} CACHE INTERNAL "pthread available on system")
