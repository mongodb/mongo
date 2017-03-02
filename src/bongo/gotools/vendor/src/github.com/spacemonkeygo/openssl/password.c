#include <openssl/ssl.h>
#include "_cgo_export.h"

int password_cb(char *buf,int buf_len, int rwflag,void *userdata) {
    char* pw = (char *)userdata;
    int l = strlen(pw);
    if (l + 1 > buf_len) return 0;
    strcpy(buf,pw);
    return l;
}
