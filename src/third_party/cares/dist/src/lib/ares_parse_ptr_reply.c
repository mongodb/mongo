/* MIT License
 *
 * Copyright (c) 2023 Brad House
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ares_setup.h"

#ifdef HAVE_NETINET_IN_H
#  include <netinet/in.h>
#endif
#ifdef HAVE_NETDB_H
#  include <netdb.h>
#endif

#include "ares.h"
#include "ares_private.h"

int ares_parse_ptr_reply(const unsigned char *abuf, int alen_int,
                         const void *addr, int addrlen, int family,
                         struct hostent **host)
{
  ares_status_t      status;
  size_t             alen;
  size_t             ptrcount = 0;
  struct hostent    *hostent  = NULL;
  const char        *hostname = NULL;
  const char        *ptrname  = NULL;
  ares_dns_record_t *dnsrec   = NULL;
  size_t             i;
  size_t             ancount;

  *host = NULL;

  if (alen_int < 0) {
    return ARES_EBADRESP;
  }

  alen = (size_t)alen_int;

  status = ares_dns_parse(abuf, alen, 0, &dnsrec);
  if (status != ARES_SUCCESS) {
    goto done;
  }

  /* Fetch name from query as we will use it to compare later on.  Old code
   * did this check, so we'll retain it. */
  status = ares_dns_record_query_get(dnsrec, 0, &ptrname, NULL, NULL);
  if (status != ARES_SUCCESS) {
    goto done;
  }

  ancount = ares_dns_record_rr_cnt(dnsrec, ARES_SECTION_ANSWER);
  if (ancount == 0) {
    status = ARES_ENODATA;
    goto done;
  }

  /* Response structure */
  hostent = ares_malloc(sizeof(*hostent));
  if (hostent == NULL) {
    status = ARES_ENOMEM;
    goto done;
  }

  memset(hostent, 0, sizeof(*hostent));

  hostent->h_addr_list = ares_malloc(2 * sizeof(*hostent->h_addr_list));
  if (hostent->h_addr_list == NULL) {
    status = ARES_ENOMEM;
    goto done;
  }
  memset(hostent->h_addr_list, 0, 2 * sizeof(*hostent->h_addr_list));
  if (addr != NULL && addrlen > 0) {
    hostent->h_addr_list[0] = ares_malloc((size_t)addrlen);
    if (hostent->h_addr_list[0] == NULL) {
      status = ARES_ENOMEM;
      goto done;
    }
    memcpy(hostent->h_addr_list[0], addr, (size_t)addrlen);
  }
  hostent->h_addrtype = (HOSTENT_ADDRTYPE_TYPE)family;
  hostent->h_length   = (HOSTENT_LENGTH_TYPE)addrlen;

  /* Preallocate the maximum number + 1 */
  hostent->h_aliases = ares_malloc((ancount + 1) * sizeof(*hostent->h_aliases));
  if (hostent->h_aliases == NULL) {
    status = ARES_ENOMEM;
    goto done;
  }
  memset(hostent->h_aliases, 0, (ancount + 1) * sizeof(*hostent->h_aliases));


  /* Cycle through answers */
  for (i = 0; i < ancount; i++) {
    const ares_dns_rr_t *rr =
      ares_dns_record_rr_get(dnsrec, ARES_SECTION_ANSWER, i);

    if (rr == NULL) {
      /* Shouldn't be possible */
      status = ARES_EBADRESP;
      goto done;
    }

    if (ares_dns_rr_get_class(rr) != ARES_CLASS_IN) {
      continue;
    }

    /* Any time we see a CNAME, replace our ptrname with its value */
    if (ares_dns_rr_get_type(rr) == ARES_REC_TYPE_CNAME) {
      ptrname = ares_dns_rr_get_str(rr, ARES_RR_CNAME_CNAME);
      if (ptrname == NULL) {
        status = ARES_EBADRESP;
        goto done;
      }
    }

    /* Handling for PTR records below this, otherwise skip */
    if (ares_dns_rr_get_type(rr) != ARES_REC_TYPE_PTR) {
      continue;
    }

    /* Issue #683
     * Old code compared the name in the rr to the ptrname, but I think this
     * is wrong since it was proven wrong for A & AAAA records.  Leaving
     * this code commented out for future reference
     *
     * rname = ares_dns_rr_get_name(rr);
     * if (rname == NULL) {
     *   status = ARES_EBADRESP;
     *   goto done;
     * }
     * if (strcasecmp(ptrname, rname) != 0) {
     *   continue;
     * }
     */

    /* Save most recent PTR record as the hostname */
    hostname = ares_dns_rr_get_str(rr, ARES_RR_PTR_DNAME);
    if (hostname == NULL) {
      status = ARES_EBADRESP;
      goto done;
    }

    /* Append as an alias */
    hostent->h_aliases[ptrcount] = ares_strdup(hostname);
    if (hostent->h_aliases[ptrcount] == NULL) {
      status = ARES_ENOMEM;
      goto done;
    }
    ptrcount++;
  }

  if (ptrcount == 0) {
    status = ARES_ENODATA;
    goto done;
  } else {
    status = ARES_SUCCESS;
  }

  /* Fill in hostname */
  hostent->h_name = ares_strdup(hostname);
  if (hostent->h_name == NULL) {
    status = ARES_ENOMEM;
    goto done;
  }

done:
  if (status != ARES_SUCCESS) {
    ares_free_hostent(hostent);
    /* Compatibility */
    if (status == ARES_EBADNAME) {
      status = ARES_EBADRESP;
    }
  } else {
    *host = hostent;
  }
  ares_dns_record_destroy(dnsrec);
  return (int)status;
}
