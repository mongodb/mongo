/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
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

#ifndef jsutil_h___
#define jsutil_h___

JS_BEGIN_EXTERN_C

#ifdef DEBUG

extern JS_PUBLIC_API(void)
JS_Assert(const char *s, const char *file, JSIntn ln);
#define JS_ASSERT(_expr) \
    ((_expr)?((void)0):JS_Assert(# _expr,__FILE__,__LINE__))

#define JS_NOT_REACHED(_reasonStr) \
    JS_Assert(_reasonStr,__FILE__,__LINE__)

#else

#define JS_ASSERT(expr) ((void) 0)
#define JS_NOT_REACHED(reasonStr)

#endif /* defined(DEBUG) */

/*
 * Compile-time assert. "condition" must be a constant expression.
 * The macro should be used only once per source line in places where
 * a "typedef" declaration is allowed.
 */
#define JS_STATIC_ASSERT(condition)                                           \
    JS_STATIC_ASSERT_IMPL(condition, __LINE__)
#define JS_STATIC_ASSERT_IMPL(condition, line)                                \
    JS_STATIC_ASSERT_IMPL2(condition, line)
#define JS_STATIC_ASSERT_IMPL2(condition, line)                               \
    typedef int js_static_assert_line_##line[(condition) ? 1 : -1]

/*
** Abort the process in a non-graceful manner. This will cause a core file,
** call to the debugger or other moral equivalent as well as causing the
** entire process to stop.
*/
extern JS_PUBLIC_API(void) JS_Abort(void);

#ifdef XP_UNIX

typedef struct JSCallsite JSCallsite;

struct JSCallsite {
    uint32      pc;
    char        *name;
    const char  *library;
    int         offset;
    JSCallsite  *parent;
    JSCallsite  *siblings;
    JSCallsite  *kids;
    void        *handy;
};

extern JSCallsite *JS_Backtrace(int skip);

#endif

JS_END_EXTERN_C

#endif /* jsutil_h___ */
