//  Copyright 2023 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

#if defined(_MSC_VER)
// MSVC's link.exe does not support -Wl,... flags, but doesn't fail the linking.
// The linker may be used by different compilers, not only MSVC.
// Luckily, those compilers all pretend to be MSVC.
#error "MSVC and compatible compilers don't support -Wl,... flags"
#endif

#if defined(__OpenBSD__)
// On OpenBSD, shared libraries are not linked to libc, as there are multiple
// libc versions installed, and loading multiple different versions into the
// process is considered dangerous. Only the main executable is linked against
// one of them, which will be used by all shared libraries loaded into the
// process. This renders -Wl,--no-undefined unusable for shared libraries.
#error "-Wl,--no-undefined is broken for shared libraries on OpenBSD"
#endif

int main()
{
    return 0;
}
