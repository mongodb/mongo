#include <time.h>
int main()
{
#ifndef clock_gettime
    (void) clock_gettime;
#endif
    ;
    return 0;
}
