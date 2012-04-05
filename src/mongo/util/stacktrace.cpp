// Copyright 2009.  10gen, Inc.

#include "mongo/util/stacktrace.h"

#include <cstdlib>
#include <iostream>

#ifdef MONGO_HAVE_EXECINFO_BACKTRACE

#include <execinfo.h>

static const int maxBackTraceFrames = 20;

void ::mongo::printStackTrace( std::ostream &os ) {

    void *b[maxBackTraceFrames];

    int size = ::backtrace( b, maxBackTraceFrames );
    for ( int i = 0; i < size; i++ )
        os << std::hex << b[i] << std::dec << ' ';
    os << std::endl;

    char **strings;

    strings = ::backtrace_symbols( b, size );
    for ( int i = 0; i < size; i++ )
        os << ' ' << strings[i] << '\n';
    os.flush();
    ::free( strings );
}

#else

void ::mongo::printStackTrace( std::ostream &os ) {}

#endif  // defined(MONGO_HAVE_EXECINFO_BACKTRACE)

