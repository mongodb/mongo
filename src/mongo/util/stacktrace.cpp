// Copyright 2009.  10gen, Inc.

#include "mongo/util/stacktrace.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <map>
#include <vector>

#ifdef MONGO_HAVE_EXECINFO_BACKTRACE

#include <execinfo.h>

namespace mongo {
    static const int maxBackTraceFrames = 20;
    
    void printStackTrace( std::ostream &os ) {
        
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
}

#elif MONGO_HAVE_UNWIND_BACKTRACE

// This makes libunwind only work for within the
// current process, but that's what we want anyway.
#define UNW_LOCAL_ONLY

#include <libunwind.h>

namespace mongo {
    static const int maxBackTraceFrames = 20;

    void printStackTrace( std::ostream &os ) {
        unw_context_t stack;
        unw_cursor_t cursor;

        int status = unw_getcontext(&stack);
        if (status != 0) {
            os << "unw_getcontext failed: " << status << std::endl;
            return;
        }
        status = unw_init_local(&cursor, &stack);
        if (status != 0) {
            os << "unw_init_local failed: " << status << std::endl;
            return;
        }
        for ( int depth = 0; (depth < maxBackTraceFrames && unw_step(&cursor) != 0); ++depth ) {
            unw_word_t  offset;
            unw_word_t  pc;
            char        fname[128];

            status = unw_get_reg(&cursor, UNW_REG_IP,  &pc);
            if (status != 0) {
                os << "unw_get_reg failed: " << status << std::endl;
                return;
            }

            fname[0] = '\0';
            status = unw_get_proc_name(&cursor, fname, sizeof(fname), &offset);
            if (status != 0) {
                os << "unw_get_proc_name failed: " << status << std::endl;
                return;
            }
            os << fname << "+0x" << offset << " [0x" << std::hex << pc << std::dec << "]" << std::endl;
        }
    }
}

#else

namespace mongo {   
    void printStackTrace( std::ostream &os ) {}
}

#endif  // defined(MONGO_HAVE_EXECINFO_BACKTRACE)

