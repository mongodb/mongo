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
#include "ares.h"
#include "ares_data.h"
#include "ares_private.h"

static int ares__parse_txt_reply(const unsigned char *abuf, size_t alen,
                                 ares_bool_t ex, void **txt_out)
{
  ares_status_t        status;
  struct ares_txt_ext *txt_head = NULL;
  struct ares_txt_ext *txt_last = NULL;
  struct ares_txt_ext *txt_curr;
  ares_dns_record_t   *dnsrec = NULL;
  size_t               i;

  *txt_out = NULL;

  status = ares_dns_parse(abuf, alen, 0, &dnsrec);
  if (status != ARES_SUCCESS) {
    goto done;
  }

  if (ares_dns_record_rr_cnt(dnsrec, ARES_SECTION_ANSWER) == 0) {
    status = ARES_ENODATA;
    goto done;
  }

  for (i = 0; i < ares_dns_record_rr_cnt(dnsrec, ARES_SECTION_ANSWER); i++) {
    const ares_dns_rr_t *rr =
      ares_dns_record_rr_get(dnsrec, ARES_SECTION_ANSWER, i);
    const unsigned char *ptr;
    size_t               ptr_len;

    if (rr == NULL) {
      /* Shouldn't be possible */
      status = ARES_EBADRESP;
      goto done;
    }

    /* XXX: Why Chaos? */
    if ((ares_dns_rr_get_class(rr) != ARES_CLASS_IN &&
         ares_dns_rr_get_class(rr) != ARES_CLASS_CHAOS) ||
        ares_dns_rr_get_type(rr) != ARES_REC_TYPE_TXT) {
      continue;
    }

    /* Allocate storage for this TXT answer appending it to the list */
    txt_curr =
      ares_malloc_data(ex ? ARES_DATATYPE_TXT_EXT : ARES_DATATYPE_TXT_REPLY);
    if (txt_curr == NULL) {
      status = ARES_ENOMEM;
      goto done;
    }

    /* Link in the record */
    if (txt_last) {
      txt_last->next = txt_curr;
    } else {
      txt_head = txt_curr;
    }
    txt_last = txt_curr;

    /* These days, records are joined, always tag as start */
    if (ex) {
      txt_curr->record_start = 1;
    }

    ptr = ares_dns_rr_get_bin(rr, ARES_RR_TXT_DATA, &ptr_len);

    txt_curr->txt = ares_malloc(ptr_len + 1);
    if (txt_curr->txt == NULL) {
      status = ARES_ENOMEM;
      goto done;
    }
    memcpy(txt_curr->txt, ptr, ptr_len);
    txt_curr->txt[ptr_len] = 0;
    txt_curr->length       = ptr_len;
  }

done:
  /* clean up on error */
  if (status != ARES_SUCCESS) {
    if (txt_head) {
      ares_free_data(txt_head);
    }
  } else {
    /* everything looks fine, return the data */
    *txt_out = txt_head;
  }
  ares_dns_record_destroy(dnsrec);
  return (int)status;
}

int ares_parse_txt_reply(const unsigned char *abuf, int alen,
                         struct ares_txt_reply **txt_out)
{
  if (alen < 0) {
    return ARES_EBADRESP;
  }
  return ares__parse_txt_reply(abuf, (size_t)alen, ARES_FALSE,
                               (void **)txt_out);
}

int ares_parse_txt_reply_ext(const unsigned char *abuf, int alen,
                             struct ares_txt_ext **txt_out)
{
  if (alen < 0) {
    return ARES_EBADRESP;
  }
  return ares__parse_txt_reply(abuf, (size_t)alen, ARES_TRUE, (void **)txt_out);
}
