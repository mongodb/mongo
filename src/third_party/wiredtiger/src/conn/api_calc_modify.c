/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#define WT_CM_BLOCKSIZE 8
#define WT_CM_MINMATCH 64
#define WT_CM_STARTGAP (WT_CM_BLOCKSIZE / 2)

typedef struct {
    WT_SESSION_IMPL *session;

    const uint8_t *s1, *e1; /* Start / end of pre-image. */
    const uint8_t *s2, *e2; /* Start / end of after-image. */

    const uint8_t *used1, *used2; /* Used up to here. */

    size_t maxdiff;
    int maxentries;
} WT_CM_STATE;

typedef struct {
    const uint8_t *m1, *m2;
    size_t len;
} WT_CM_MATCH;

/*
 * __cm_add_modify --
 *     Add a modify operation to the list of entries. Fails if all entries are used or the maximum
 *     bytes of difference is exceeded.
 */
static int
__cm_add_modify(WT_CM_STATE *cms, const uint8_t *p2, const uint8_t *m1, const uint8_t *m2,
  WT_MODIFY *entries, int *nentriesp)
{
    WT_MODIFY *mod;
    size_t len1, len2;

    WT_ASSERT(cms->session, m1 >= cms->used1 && m2 >= cms->used2);

    len1 = (size_t)(m1 - cms->used1);
    len2 = (size_t)(m2 - cms->used2);

    if (*nentriesp >= cms->maxentries || len2 > cms->maxdiff)
        return (WT_NOTFOUND);

    mod = entries + (*nentriesp)++;
    mod->offset = (size_t)(p2 - cms->s2);
    mod->size = len1;
    mod->data.data = p2;
    mod->data.size = len2;
    cms->maxdiff -= len2;

    return (0);
}

/*
 * __cm_extend --
 *     Given a potential match size, extend to find the complete match.
 */
static void
__cm_extend(WT_CM_STATE *cms, const uint8_t *m1, const uint8_t *m2, WT_CM_MATCH *match)
{
    ptrdiff_t n;
    const uint8_t *p1, *p2;

    /*
     * Keep skipping half of the remaining bytes while they compare equal. This is significantly
     * faster than our byte-at-a-time loop below.
     */
    for (p1 = m1, p2 = m2;
         (n = WT_MIN(cms->e1 - p1, cms->e2 - p2) / 2) > 8 && memcmp(p1, p2, (size_t)n) == 0;
         p1 += n, p2 += n)
        ;

    /* Step past the end and before the beginning of the matching block. */
    for (n = WT_MIN(cms->e1 - p1, cms->e2 - p2); n > 0 && *p1 == *p2; n--, p1++, p2++)
        ;

    for (n = WT_MIN(m1 - cms->used1, m2 - cms->used2); n > 0 && *m1 == *m2; n--, m1--, m2--)
        ;

    match->m1 = m1 + 1;
    match->m2 = m2 + 1;
    match->len = p1 > m1 ? (size_t)((p1 - m1) - 1) : 0;
}

/*
 * __cm_fingerprint --
 *     Calculate an integral "fingerprint" of a block of bytes.
 */
static inline uint64_t
__cm_fingerprint(const uint8_t *p)
{
    uint64_t h;

    WT_STATIC_ASSERT(sizeof(h) <= WT_CM_BLOCKSIZE);
    memcpy(&h, p, WT_CM_BLOCKSIZE);
    return (h);
}

/*
 * __wt_calc_modify --
 *     Calculate a set of WT_MODIFY operations to represent an update.
 */
int
__wt_calc_modify(WT_SESSION_IMPL *wt_session, const WT_ITEM *oldv, const WT_ITEM *newv,
  size_t maxdiff, WT_MODIFY *entries, int *nentriesp)
{
    WT_CM_MATCH match;
    WT_CM_STATE cms;
    size_t gap, i;
    uint64_t h, hend, hstart;
    const uint8_t *p1, *p2;
    bool start;

    if (oldv->size < WT_CM_MINMATCH || newv->size < WT_CM_MINMATCH)
        return (WT_NOTFOUND);

    cms.session = (WT_SESSION_IMPL *)wt_session;

    cms.s1 = cms.used1 = oldv->data;
    cms.e1 = cms.s1 + oldv->size;
    cms.s2 = cms.used2 = newv->data;
    cms.e2 = cms.s2 + newv->size;
    cms.maxdiff = maxdiff;
    cms.maxentries = *nentriesp;
    *nentriesp = 0;

    /* Ignore matches at the beginning / end. */
    __cm_extend(&cms, cms.s1, cms.s2, &match);
    cms.used1 += match.len;
    cms.used2 += match.len;
    if (cms.used1 < cms.e1 && cms.used2 < cms.e2) {
        __cm_extend(&cms, cms.e1 - 1, cms.e2 - 1, &match);
        cms.e1 -= match.len;
        cms.e2 -= match.len;
    }

    if (cms.used1 + WT_CM_BLOCKSIZE >= cms.e1 || cms.used2 + WT_CM_BLOCKSIZE >= cms.e2)
        goto end;

    /*
     * Walk through the post-image, maintaining start / end markers separated by a gap in the
     * pre-image. If the current point in the post-image matches either marker, try to extend the
     * match to find a (large) range of matching bytes. If the end of the range is reached in the
     * post-image without finding a good match, double the size of the gap, update the markers and
     * keep trying.
     */
    hstart = hend = 0;
    i = gap = 0;
    for (p1 = cms.used1, p2 = cms.used2, start = true;
         p1 + WT_CM_BLOCKSIZE <= cms.e1 && p2 + WT_CM_BLOCKSIZE <= cms.e2; p2++, i++) {
        if (start || i == gap) {
            p1 += gap;
            gap = start ? WT_CM_STARTGAP : gap * 2;
            if (p1 + gap + WT_CM_BLOCKSIZE >= cms.e1)
                break;
            if (gap > maxdiff)
                return (WT_NOTFOUND);
            hstart = start ? __cm_fingerprint(p1) : hend;
            hend = __cm_fingerprint(p1 + gap);
            start = false;
            i = 0;
        }
        h = __cm_fingerprint(p2);
        match.len = 0;
        if (h == hstart)
            __cm_extend(&cms, p1, p2, &match);
        else if (h == hend)
            __cm_extend(&cms, p1 + gap, p2, &match);

        if (match.len < WT_CM_MINMATCH)
            continue;

        WT_RET(__cm_add_modify(&cms, cms.used2, match.m1, match.m2, entries, nentriesp));
        cms.used1 = p1 = match.m1 + match.len;
        cms.used2 = p2 = match.m2 + match.len;
        start = true;
    }

end:
    if (cms.used1 < cms.e1 || cms.used2 < cms.e2)
        WT_RET(__cm_add_modify(&cms, cms.used2, cms.e1, cms.e2, entries, nentriesp));

    return (0);
}

/*
 * wiredtiger_calc_modify --
 *     Calculate a set of WT_MODIFY operations to represent an update.
 */
int
wiredtiger_calc_modify(WT_SESSION *wt_session, const WT_ITEM *oldv, const WT_ITEM *newv,
  size_t maxdiff, WT_MODIFY *entries, int *nentriesp)
{
    return (
      __wt_calc_modify((WT_SESSION_IMPL *)wt_session, oldv, newv, maxdiff, entries, nentriesp));
}
