#include <execinfo.h>
int main()
{
#ifndef backtrace
    (void) backtrace;
#endif
    ;
    return 0;
}
