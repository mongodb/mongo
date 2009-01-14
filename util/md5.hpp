// md5.hpp

#pragma once

#include "md5.h"

namespace mongo {

typedef unsigned char md5digest[16];

inline void md5(const void *buf, int nbytes, md5digest digest) {
    md5_state_t st;
    md5_init(&st);
    md5_append(&st, (const md5_byte_t *) buf, nbytes);
    md5_finish(&st, digest);
}

inline void md5(const char *str, md5digest digest) {
    md5(str, strlen(str), digest);
}

} // namespace mongo
