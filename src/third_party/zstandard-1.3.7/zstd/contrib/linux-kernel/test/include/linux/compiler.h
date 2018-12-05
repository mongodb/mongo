#ifndef LINUX_COMIPLER_H_
#define LINUX_COMIPLER_H_

#ifndef __always_inline
#  define __always_inline inline
#endif

#ifndef noinline
#  define noinline __attribute__((__noinline__))
#endif

#endif // LINUX_COMIPLER_H_
