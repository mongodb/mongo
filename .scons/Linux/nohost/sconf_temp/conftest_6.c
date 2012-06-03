#include <execinfo.h>
int main()
{
#ifndef backtrace_symbols
    (void) backtrace_symbols;
#endif
    ;
    return 0;
}
