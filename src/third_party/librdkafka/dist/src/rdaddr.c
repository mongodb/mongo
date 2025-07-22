/*
 * librd - Rapid Development C library
 *
 * Copyright (c) 2012-2022, Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */



#include "rd.h"
#include "rdaddr.h"
#include "rdrand.h"

#ifdef _WIN32
#include <ws2tcpip.h>
#endif

const char *rd_sockaddr2str(const void *addr, int flags) {
        const rd_sockaddr_inx_t *a = (const rd_sockaddr_inx_t *)addr;
        static RD_TLS char ret[32][256];
        static RD_TLS int reti = 0;
        char portstr[32];
        int of      = 0;
        int niflags = NI_NUMERICSERV;
        int r;

        reti = (reti + 1) % 32;

        switch (a->sinx_family) {
        case AF_INET:
        case AF_INET6:
                if (flags & RD_SOCKADDR2STR_F_FAMILY)
                        of += rd_snprintf(&ret[reti][of],
                                          sizeof(ret[reti]) - of, "ipv%i#",
                                          a->sinx_family == AF_INET ? 4 : 6);

                if ((flags & RD_SOCKADDR2STR_F_PORT) &&
                    a->sinx_family == AF_INET6)
                        ret[reti][of++] = '[';

                if (!(flags & RD_SOCKADDR2STR_F_RESOLVE))
                        niflags |= NI_NUMERICHOST;

        retry:
                if ((r = getnameinfo(
                         (const struct sockaddr *)a, RD_SOCKADDR_INX_LEN(a),

                         ret[reti] + of, sizeof(ret[reti]) - of,

                         (flags & RD_SOCKADDR2STR_F_PORT) ? portstr : NULL,

                         (flags & RD_SOCKADDR2STR_F_PORT) ? sizeof(portstr) : 0,

                         niflags))) {

                        if (r == EAI_AGAIN && !(niflags & NI_NUMERICHOST)) {
                                /* If unable to resolve name, retry without
                                 * name resolution. */
                                niflags |= NI_NUMERICHOST;
                                goto retry;
                        }
                        break;
                }


                if (flags & RD_SOCKADDR2STR_F_PORT) {
                        size_t len = strlen(ret[reti]);
                        rd_snprintf(
                            ret[reti] + len, sizeof(ret[reti]) - len, "%s:%s",
                            a->sinx_family == AF_INET6 ? "]" : "", portstr);
                }

                return ret[reti];
        }


        /* Error-case */
        rd_snprintf(ret[reti], sizeof(ret[reti]), "<unsupported:%s>",
                    rd_family2str(a->sinx_family));

        return ret[reti];
}


const char *rd_addrinfo_prepare(const char *nodesvc, char **node, char **svc) {
        static RD_TLS char snode[256];
        static RD_TLS char ssvc[64];
        const char *t;
        const char *svct = NULL;
        size_t nodelen   = 0;

        *snode = '\0';
        *ssvc  = '\0';

        if (*nodesvc == '[') {
                /* "[host]".. (enveloped node name) */
                if (!(t = strchr(nodesvc, ']')))
                        return "Missing close-']'";
                nodesvc++;
                nodelen = t - nodesvc;
                svct    = t + 1;

        } else if (*nodesvc == ':' && *(nodesvc + 1) != ':') {
                /* ":"..  (port only) */
                nodelen = 0;
                svct    = nodesvc;
        }

        if ((svct = strrchr(svct ? svct : nodesvc, ':')) &&
            (*(svct - 1) != ':') && *(++svct)) {
                /* Optional ":service" definition. */
                if (strlen(svct) >= sizeof(ssvc))
                        return "Service name too long";
                strcpy(ssvc, svct);
                if (!nodelen)
                        nodelen = svct - nodesvc - 1;

        } else if (!nodelen)
                nodelen = strlen(nodesvc);

        if (nodelen) {
                /* Truncate nodename if necessary. */
                nodelen = RD_MIN(nodelen, sizeof(snode) - 1);
                memcpy(snode, nodesvc, nodelen);
                snode[nodelen] = '\0';
        }

        *node = snode;
        *svc  = ssvc;

        return NULL;
}



rd_sockaddr_list_t *
rd_getaddrinfo(const char *nodesvc,
               const char *defsvc,
               int flags,
               int family,
               int socktype,
               int protocol,
               int (*resolve_cb)(const char *node,
                                 const char *service,
                                 const struct addrinfo *hints,
                                 struct addrinfo **res,
                                 void *opaque),
               void *opaque,
               const char **errstr) {
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = family;
        hints.ai_socktype = socktype;
        hints.ai_protocol = protocol;
        hints.ai_flags    = flags;

        struct addrinfo *ais, *ai;
        char *node, *svc;
        int r;
        int cnt = 0;
        rd_sockaddr_list_t *rsal;

        if ((*errstr = rd_addrinfo_prepare(nodesvc, &node, &svc))) {
                errno = EINVAL;
                return NULL;
        }

        if (*svc)
                defsvc = svc;

        if (resolve_cb) {
                r = resolve_cb(node, defsvc, &hints, &ais, opaque);
        } else {
                r = getaddrinfo(node, defsvc, &hints, &ais);
        }

        if (r) {
#ifdef EAI_SYSTEM
                if (r == EAI_SYSTEM)
#else
                if (0)
#endif
                        *errstr = rd_strerror(errno);
                else {
#ifdef _WIN32
                        *errstr = gai_strerrorA(r);
#else
                        *errstr = gai_strerror(r);
#endif
                        errno = EFAULT;
                }
                return NULL;
        }

        /* Count number of addresses */
        for (ai = ais; ai != NULL; ai = ai->ai_next)
                cnt++;

        if (cnt == 0) {
                /* unlikely? */
                if (resolve_cb)
                        resolve_cb(NULL, NULL, NULL, &ais, opaque);
                else
                        freeaddrinfo(ais);
                errno   = ENOENT;
                *errstr = "No addresses";
                return NULL;
        }


        rsal = rd_calloc(1, sizeof(*rsal) + (sizeof(*rsal->rsal_addr) * cnt));

        for (ai = ais; ai != NULL; ai = ai->ai_next)
                memcpy(&rsal->rsal_addr[rsal->rsal_cnt++], ai->ai_addr,
                       ai->ai_addrlen);

        if (resolve_cb)
                resolve_cb(NULL, NULL, NULL, &ais, opaque);
        else
                freeaddrinfo(ais);

        /* Shuffle address list for proper round-robin */
        if (!(flags & RD_AI_NOSHUFFLE))
                rd_array_shuffle(rsal->rsal_addr, rsal->rsal_cnt,
                                 sizeof(*rsal->rsal_addr));

        return rsal;
}



void rd_sockaddr_list_destroy(rd_sockaddr_list_t *rsal) {
        rd_free(rsal);
}
