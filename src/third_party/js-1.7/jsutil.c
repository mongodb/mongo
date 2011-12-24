/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Communicator client code, released
 * March 31, 1998.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   IBM Corp.
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

/*
 * PR assertion checker.
 */
#include "jsstddef.h"
#include <stdio.h>
#include <stdlib.h>
#include "jstypes.h"
#include "jsutil.h"

#ifdef WIN32
#    include <windows.h>
#endif

JS_PUBLIC_API(void) JS_Assert(const char *s, const char *file, JSIntn ln)
{
    fprintf(stderr, "Assertion failure: %s, at %s:%d\n", s, file, ln);
#if defined(WIN32)
    DebugBreak();
    exit(3);
#elif defined(XP_OS2) || (defined(__GNUC__) && defined(__i386))
    asm("int $3");
#endif
    abort();
}

#if defined DEBUG_notme && defined XP_UNIX

#define __USE_GNU 1
#include <dlfcn.h>
#include <string.h>
#include "jshash.h"
#include "jsprf.h"

JSCallsite js_calltree_root = {0, NULL, NULL, 0, NULL, NULL, NULL, NULL};

static JSCallsite *
CallTree(void **bp)
{
    void **bpup, **bpdown, *pc;
    JSCallsite *parent, *site, **csp;
    Dl_info info;
    int ok, offset;
    const char *symbol;
    char *method;

    /* Reverse the stack frame list to avoid recursion. */
    bpup = NULL;
    for (;;) {
        bpdown = (void**) bp[0];
        bp[0] = (void*) bpup;
        if ((void**) bpdown[0] < bpdown)
            break;
        bpup = bp;
        bp = bpdown;
    }

    /* Reverse the stack again, finding and building a path in the tree. */
    parent = &js_calltree_root;
    do {
        bpup = (void**) bp[0];
        bp[0] = (void*) bpdown;
        pc = bp[1];

        csp = &parent->kids;
        while ((site = *csp) != NULL) {
            if (site->pc == pc) {
                /* Put the most recently used site at the front of siblings. */
                *csp = site->siblings;
                site->siblings = parent->kids;
                parent->kids = site;

                /* Site already built -- go up the stack. */
                goto upward;
            }
            csp = &site->siblings;
        }

        /* Check for recursion: see if pc is on our ancestor line. */
        for (site = parent; site; site = site->parent) {
            if (site->pc == pc)
                goto upward;
        }

        /*
         * Not in tree at all: let's find our symbolic callsite info.
         * XXX static syms are masked by nearest lower global
         */
        info.dli_fname = info.dli_sname = NULL;
        ok = dladdr(pc, &info);
        if (ok < 0) {
            fprintf(stderr, "dladdr failed!\n");
            return NULL;
        }

/* XXXbe sub 0x08040000? or something, see dbaron bug with tenthumbs comment */
        symbol = info.dli_sname;
        offset = (char*)pc - (char*)info.dli_fbase;
        method = symbol
                 ? strdup(symbol)
                 : JS_smprintf("%s+%X",
                               info.dli_fname ? info.dli_fname : "main",
                               offset);
        if (!method)
            return NULL;

        /* Create a new callsite record. */
        site = (JSCallsite *) malloc(sizeof(JSCallsite));
        if (!site)
            return NULL;

        /* Insert the new site into the tree. */
        site->pc = pc;
        site->name = method;
        site->library = info.dli_fname;
        site->offset = offset;
        site->parent = parent;
        site->siblings = parent->kids;
        parent->kids = site;
        site->kids = NULL;

      upward:
        parent = site;
        bpdown = bp;
        bp = bpup;
    } while (bp);

    return site;
}

JSCallsite *
JS_Backtrace(int skip)
{
    void **bp, **bpdown;

    /* Stack walking code adapted from Kipp's "leaky". */
#if defined(__i386)
    __asm__( "movl %%ebp, %0" : "=g"(bp));
#elif defined(__x86_64__)
    __asm__( "movq %%rbp, %0" : "=g"(bp));
#else
    /*
     * It would be nice if this worked uniformly, but at least on i386 and
     * x86_64, it stopped working with gcc 4.1, because it points to the
     * end of the saved registers instead of the start.
     */
    bp = (void**) __builtin_frame_address(0);
#endif
    while (--skip >= 0) {
        bpdown = (void**) *bp++;
        if (bpdown < bp)
            break;
        bp = bpdown;
    }

    return CallTree(bp);
}

#endif /* DEBUG_notme && XP_UNIX */
