/*
 * librd - Rapid Development C library
 *
 * Copyright (c) 2012-2022, Magnus Edenhill
 * Copyright (c) 2012-2022, Andreas Öman
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


/*

 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _RDSYSQUEUE_H_
#define _RDSYSQUEUE_H_

#include "queue.h"

/*
 * Complete missing LIST-ops
 */

#ifndef LIST_FOREACH
#define LIST_FOREACH(var, head, field)                                         \
        for ((var) = ((head)->lh_first); (var); (var) = ((var)->field.le_next))
#endif

#ifndef LIST_EMPTY
#define LIST_EMPTY(head) ((head)->lh_first == NULL)
#endif

#ifndef LIST_FIRST
#define LIST_FIRST(head) ((head)->lh_first)
#endif

#ifndef LIST_NEXT
#define LIST_NEXT(elm, field) ((elm)->field.le_next)
#endif

#ifndef LIST_INSERT_BEFORE
#define LIST_INSERT_BEFORE(listelm, elm, field)                                \
        do {                                                                   \
                (elm)->field.le_prev      = (listelm)->field.le_prev;          \
                (elm)->field.le_next      = (listelm);                         \
                *(listelm)->field.le_prev = (elm);                             \
                (listelm)->field.le_prev  = &(elm)->field.le_next;             \
        } while (/*CONSTCOND*/ 0)
#endif

/*
 * Complete missing TAILQ-ops
 */

#ifndef TAILQ_HEAD_INITIALIZER
#define TAILQ_HEAD_INITIALIZER(head)                                           \
        { NULL, &(head).tqh_first }
#endif

#ifndef TAILQ_INSERT_BEFORE
#define TAILQ_INSERT_BEFORE(listelm, elm, field)                               \
        do {                                                                   \
                (elm)->field.tqe_prev      = (listelm)->field.tqe_prev;        \
                (elm)->field.tqe_next      = (listelm);                        \
                *(listelm)->field.tqe_prev = (elm);                            \
                (listelm)->field.tqe_prev  = &(elm)->field.tqe_next;           \
        } while (0)
#endif

#ifndef TAILQ_FOREACH
#define TAILQ_FOREACH(var, head, field)                                        \
        for ((var) = ((head)->tqh_first); (var);                               \
             (var) = ((var)->field.tqe_next))
#endif

#ifndef TAILQ_EMPTY
#define TAILQ_EMPTY(head) ((head)->tqh_first == NULL)
#endif

#ifndef TAILQ_FIRST
#define TAILQ_FIRST(head) ((head)->tqh_first)
#endif

#ifndef TAILQ_NEXT
#define TAILQ_NEXT(elm, field) ((elm)->field.tqe_next)
#endif

#ifndef TAILQ_LAST
#define TAILQ_LAST(head, headname)                                             \
        (*(((struct headname *)((head)->tqh_last))->tqh_last))
#endif

#ifndef TAILQ_PREV
#define TAILQ_PREV(elm, headname, field)                                       \
        (*(((struct headname *)((elm)->field.tqe_prev))->tqh_last))
#endif

#ifndef TAILQ_FOREACH_SAFE
/*
 * TAILQ_FOREACH_SAFE() provides a traversal where the current iterated element
 * may be freed or unlinked.
 * It does not allow freeing or modifying any other element in the list,
 * at least not the next element.
 */
#define TAILQ_FOREACH_SAFE(elm, head, field, tmpelm)                           \
        for ((elm) = TAILQ_FIRST(head);                                        \
             (elm) && ((tmpelm) = TAILQ_NEXT((elm), field), 1);                \
             (elm) = (tmpelm))
#endif

/*
 * In Mac OS 10.4 and earlier TAILQ_FOREACH_REVERSE was defined
 * differently, redefined it.
 */
#ifdef __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__
#if __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 1050
#undef TAILQ_FOREACH_REVERSE
#endif
#endif

#ifndef TAILQ_FOREACH_REVERSE
#define TAILQ_FOREACH_REVERSE(var, head, headname, field)                      \
        for ((var) = (*(((struct headname *)((head)->tqh_last))->tqh_last));   \
             (var);                                                            \
             (var) =                                                           \
                 (*(((struct headname *)((var)->field.tqe_prev))->tqh_last)))
#endif


/**
 * Treat the TAILQ as a circular list and return the previous/next entry,
 * possibly wrapping to the end/beginning.
 */
#define TAILQ_CIRC_PREV(var, head, headname, field)                            \
        ((var) != TAILQ_FIRST(head) ? TAILQ_PREV(var, headname, field)         \
                                    : TAILQ_LAST(head, headname))

#define TAILQ_CIRC_NEXT(var, head, headname, field)                            \
        ((var) != TAILQ_LAST(head, headname) ? TAILQ_NEXT(var, field)          \
                                             : TAILQ_FIRST(head))

/*
 * Some extra functions for LIST manipulation
 */

#define LIST_INSERT_SORTED(head, elm, elmtype, field, cmpfunc)                 \
        do {                                                                   \
                if (LIST_EMPTY(head)) {                                        \
                        LIST_INSERT_HEAD(head, elm, field);                    \
                } else {                                                       \
                        elmtype _tmp;                                          \
                        LIST_FOREACH(_tmp, head, field) {                      \
                                if (cmpfunc(elm, _tmp) < 0) {                  \
                                        LIST_INSERT_BEFORE(_tmp, elm, field);  \
                                        break;                                 \
                                }                                              \
                                if (!LIST_NEXT(_tmp, field)) {                 \
                                        LIST_INSERT_AFTER(_tmp, elm, field);   \
                                        break;                                 \
                                }                                              \
                        }                                                      \
                }                                                              \
        } while (0)

#ifndef TAILQ_INSERT_SORTED
#define TAILQ_INSERT_SORTED(head, elm, elmtype, field, cmpfunc)                \
        do {                                                                   \
                if (TAILQ_FIRST(head) == NULL) {                               \
                        TAILQ_INSERT_HEAD(head, elm, field);                   \
                } else {                                                       \
                        elmtype _tmp;                                          \
                        TAILQ_FOREACH(_tmp, head, field) {                     \
                                if (cmpfunc(elm, _tmp) < 0) {                  \
                                        TAILQ_INSERT_BEFORE(_tmp, elm, field); \
                                        break;                                 \
                                }                                              \
                                if (!TAILQ_NEXT(_tmp, field)) {                \
                                        TAILQ_INSERT_AFTER(head, _tmp, elm,    \
                                                           field);             \
                                        break;                                 \
                                }                                              \
                        }                                                      \
                }                                                              \
        } while (0)
#endif

/**
 * @brief Add all elements from \p srchead to \p dsthead using sort
 *        comparator \p cmpfunc.
 *        \p src will be re-initialized on completion.
 */
#define TAILQ_CONCAT_SORTED(dsthead, srchead, elmtype, field, cmpfunc)         \
        do {                                                                   \
                elmtype _cstmp;                                                \
                elmtype _cstmp2;                                               \
                if (TAILQ_EMPTY(dsthead)) {                                    \
                        TAILQ_CONCAT(dsthead, srchead, field);                 \
                        break;                                                 \
                }                                                              \
                TAILQ_FOREACH_SAFE(_cstmp, srchead, field, _cstmp2) {          \
                        TAILQ_INSERT_SORTED(dsthead, _cstmp, elmtype, field,   \
                                            cmpfunc);                          \
                }                                                              \
                TAILQ_INIT(srchead);                                           \
        } while (0)

#define TAILQ_MOVE(newhead, oldhead, field)                                    \
        do {                                                                   \
                if (TAILQ_FIRST(oldhead)) {                                    \
                        TAILQ_FIRST(oldhead)->field.tqe_prev =                 \
                            &(newhead)->tqh_first;                             \
                        (newhead)->tqh_first = (oldhead)->tqh_first;           \
                        (newhead)->tqh_last  = (oldhead)->tqh_last;            \
                        TAILQ_INIT(oldhead);                                   \
                } else                                                         \
                        TAILQ_INIT(newhead);                                   \
        } while (/*CONSTCOND*/ 0)


/* @brief Prepend \p shead to \p dhead */
#define TAILQ_PREPEND(dhead, shead, headname, field)                           \
        do {                                                                   \
                if (unlikely(TAILQ_EMPTY(dhead))) {                            \
                        TAILQ_MOVE(dhead, shead, field);                       \
                } else if (likely(!TAILQ_EMPTY(shead))) {                      \
                        TAILQ_LAST(shead, headname)->field.tqe_next =          \
                            TAILQ_FIRST(dhead);                                \
                        TAILQ_FIRST(dhead)->field.tqe_prev =                   \
                            &TAILQ_LAST(shead, headname)->field.tqe_next;      \
                        TAILQ_FIRST(shead)->field.tqe_prev =                   \
                            &(dhead)->tqh_first;                               \
                        TAILQ_FIRST(dhead) = TAILQ_FIRST(shead);               \
                        TAILQ_INIT(shead);                                     \
                }                                                              \
        } while (0)

/* @brief Insert \p shead after element \p listelm in \p dhead */
#define TAILQ_INSERT_LIST(dhead, listelm, shead, headname, elmtype, field)       \
        do {                                                                     \
                if (TAILQ_LAST(dhead, headname) == listelm) {                    \
                        TAILQ_CONCAT(dhead, shead, field);                       \
                } else {                                                         \
                        elmtype _elm              = TAILQ_FIRST(shead);          \
                        elmtype _last             = TAILQ_LAST(shead, headname); \
                        elmtype _aft              = TAILQ_NEXT(listelm, field);  \
                        (listelm)->field.tqe_next = _elm;                        \
                        _elm->field.tqe_prev      = &(listelm)->field.tqe_next;  \
                        _last->field.tqe_next     = _aft;                        \
                        _aft->field.tqe_prev      = &_last->field.tqe_next;      \
                        TAILQ_INIT((shead));                                     \
                }                                                                \
        } while (0)

/* @brief Insert \p shead before element \p listelm in \p dhead */
#define TAILQ_INSERT_LIST_BEFORE(dhead, insert_before, shead, headname,        \
                                 elmtype, field)                               \
        do {                                                                   \
                if (TAILQ_FIRST(dhead) == insert_before) {                     \
                        TAILQ_PREPEND(dhead, shead, headname, field);          \
                } else {                                                       \
                        elmtype _first = TAILQ_FIRST(shead);                   \
                        elmtype _last  = TAILQ_LAST(shead, headname);          \
                        elmtype _dprev =                                       \
                            TAILQ_PREV(insert_before, headname, field);        \
                        _last->field.tqe_next  = insert_before;                \
                        _dprev->field.tqe_next = _first;                       \
                        (insert_before)->field.tqe_prev =                      \
                            &_last->field.tqe_next;                            \
                        _first->field.tqe_prev = &(_dprev)->field.tqe_next;    \
                        TAILQ_INIT((shead));                                   \
                }                                                              \
        } while (0)

#ifndef SIMPLEQ_HEAD
#define SIMPLEQ_HEAD(name, type)                                               \
        struct name {                                                          \
                struct type *sqh_first;                                        \
                struct type **sqh_last;                                        \
        }
#endif

#ifndef SIMPLEQ_ENTRY
#define SIMPLEQ_ENTRY(type)                                                    \
        struct {                                                               \
                struct type *sqe_next;                                         \
        }
#endif

#ifndef SIMPLEQ_FIRST
#define SIMPLEQ_FIRST(head) ((head)->sqh_first)
#endif

#ifndef SIMPLEQ_REMOVE_HEAD
#define SIMPLEQ_REMOVE_HEAD(head, field)                                       \
        do {                                                                   \
                if (((head)->sqh_first = (head)->sqh_first->field.sqe_next) == \
                    NULL)                                                      \
                        (head)->sqh_last = &(head)->sqh_first;                 \
        } while (0)
#endif

#ifndef SIMPLEQ_INSERT_TAIL
#define SIMPLEQ_INSERT_TAIL(head, elm, field)                                  \
        do {                                                                   \
                (elm)->field.sqe_next = NULL;                                  \
                *(head)->sqh_last     = (elm);                                 \
                (head)->sqh_last      = &(elm)->field.sqe_next;                \
        } while (0)
#endif

#ifndef SIMPLEQ_INIT
#define SIMPLEQ_INIT(head)                                                     \
        do {                                                                   \
                (head)->sqh_first = NULL;                                      \
                (head)->sqh_last  = &(head)->sqh_first;                        \
        } while (0)
#endif

#ifndef SIMPLEQ_INSERT_HEAD
#define SIMPLEQ_INSERT_HEAD(head, elm, field)                                  \
        do {                                                                   \
                if (((elm)->field.sqe_next = (head)->sqh_first) == NULL)       \
                        (head)->sqh_last = &(elm)->field.sqe_next;             \
                (head)->sqh_first = (elm);                                     \
        } while (0)
#endif

#ifndef SIMPLEQ_FOREACH
#define SIMPLEQ_FOREACH(var, head, field)                                      \
        for ((var) = SIMPLEQ_FIRST(head); (var) != SIMPLEQ_END(head);          \
             (var) = SIMPLEQ_NEXT(var, field))
#endif

#ifndef SIMPLEQ_INSERT_AFTER
#define SIMPLEQ_INSERT_AFTER(head, listelm, elm, field)                        \
        do {                                                                   \
                if (((elm)->field.sqe_next = (listelm)->field.sqe_next) ==     \
                    NULL)                                                      \
                        (head)->sqh_last = &(elm)->field.sqe_next;             \
                (listelm)->field.sqe_next = (elm);                             \
        } while (0)
#endif

#ifndef SIMPLEQ_END
#define SIMPLEQ_END(head) NULL
#endif

#ifndef SIMPLEQ_NEXT
#define SIMPLEQ_NEXT(elm, field) ((elm)->field.sqe_next)
#endif

#ifndef SIMPLEQ_HEAD_INITIALIZER
#define SIMPLEQ_HEAD_INITIALIZER(head)                                         \
        { NULL, &(head).sqh_first }
#endif

#ifndef SIMPLEQ_EMPTY
#define SIMPLEQ_EMPTY(head) (SIMPLEQ_FIRST(head) == SIMPLEQ_END(head))
#endif



#endif /* _RDSYSQUEUE_H_ */
