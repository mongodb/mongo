/* MIT License
 *
 * Copyright (c) 1998, 2011 Massachusetts Institute of Technology
 * Copyright (c) The c-ares project and its contributors
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

#include "ares_nameser.h"

#include "ares.h"
#include "ares_private.h" /* for the memdebug */

ares_status_t ares__expand_name_validated(const unsigned char *encoded,
                                          const unsigned char *abuf,
                                          size_t alen, char **s, size_t *enclen,
                                          ares_bool_t is_hostname)
{
  ares_status_t status;
  ares__buf_t  *buf = NULL;
  size_t        start_len;

  if (encoded == NULL || abuf == NULL || alen == 0 || enclen == NULL) {
    return ARES_EBADNAME; /* EFORMERR would be better */
  }

  if (encoded < abuf || encoded >= abuf + alen) {
    return ARES_EBADNAME; /* EFORMERR would be better */
  }

  *enclen = 0;

  /* NOTE: we allow 's' to be NULL to skip it */
  if (s) {
    *s = NULL;
  }

  buf = ares__buf_create_const(abuf, alen);

  if (buf == NULL) {
    return ARES_ENOMEM;
  }

  status = ares__buf_set_position(buf, (size_t)(encoded - abuf));
  if (status != ARES_SUCCESS) {
    goto done;
  }

  start_len = ares__buf_len(buf);
  status    = ares__dns_name_parse(buf, s, is_hostname);
  if (status != ARES_SUCCESS) {
    goto done;
  }

  *enclen = start_len - ares__buf_len(buf);

done:
  ares__buf_destroy(buf);
  return status;
}

int ares_expand_name(const unsigned char *encoded, const unsigned char *abuf,
                     int alen, char **s, long *enclen)
{
  /* Keep public API compatible */
  size_t        enclen_temp = 0;
  ares_status_t status;

  if (alen < 0) {
    return ARES_EBADRESP;
  }

  status  = ares__expand_name_validated(encoded, abuf, (size_t)alen, s,
                                        &enclen_temp, ARES_FALSE);
  *enclen = (long)enclen_temp;
  return (int)status;
}

/* Like ares_expand_name_validated  but returns EBADRESP in case of invalid
 * input. */
ares_status_t ares__expand_name_for_response(const unsigned char *encoded,
                                             const unsigned char *abuf,
                                             size_t alen, char **s,
                                             size_t     *enclen,
                                             ares_bool_t is_hostname)
{
  ares_status_t status =
    ares__expand_name_validated(encoded, abuf, alen, s, enclen, is_hostname);
  if (status == ARES_EBADNAME) {
    status = ARES_EBADRESP;
  }
  return status;
}
