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

#ifndef jsprvtd_h___
#define jsprvtd_h___
/*
 * JS private typename definitions.
 *
 * This header is included only in other .h files, for convenience and for
 * simplicity of type naming.  The alternative for structures is to use tags,
 * which are named the same as their typedef names (legal in C/C++, and less
 * noisy than suffixing the typedef name with "Struct" or "Str").  Instead,
 * all .h files that include this file may use the same typedef name, whether
 * declaring a pointer to struct type, or defining a member of struct type.
 *
 * A few fundamental scalar types are defined here too.  Neither the scalar
 * nor the struct typedefs should change much, therefore the nearly-global
 * make dependency induced by this file should not prove painful.
 */

#include "jspubtd.h"

/* Internal identifier (jsid) macros. */
#define JSID_ATOM                   0x0
#define JSID_INT                    0x1
#define JSID_OBJECT                 0x2
#define JSID_TAGMASK                0x3
#define JSID_TAG(id)                ((id) & JSID_TAGMASK)
#define JSID_SETTAG(id,t)           ((id) | (t))
#define JSID_CLRTAG(id)             ((id) & ~(jsid)JSID_TAGMASK)

#define JSID_IS_ATOM(id)            (JSID_TAG(id) == JSID_ATOM)
#define JSID_TO_ATOM(id)            ((JSAtom *)(id))
#define ATOM_TO_JSID(atom)          ((jsid)(atom))
#define ATOM_JSID_TO_JSVAL(id)      ATOM_KEY(JSID_TO_ATOM(id))

#define JSID_IS_INT(id)             ((id) & JSID_INT)
#define JSID_TO_INT(id)             ((jsint)(id) >> 1)
#define INT_TO_JSID(i)              (((jsint)(i) << 1) | JSID_INT)
#define INT_JSID_TO_JSVAL(id)       (id)
#define INT_JSVAL_TO_JSID(v)        (v)

#define JSID_IS_OBJECT(id)          (JSID_TAG(id) == JSID_OBJECT)
#define JSID_TO_OBJECT(id)          ((JSObject *) JSID_CLRTAG(id))
#define OBJECT_TO_JSID(obj)         ((jsid)(obj) | JSID_OBJECT)
#define OBJECT_JSID_TO_JSVAL(id)    OBJECT_TO_JSVAL(JSID_CLRTAG(id))
#define OBJECT_JSVAL_TO_JSID(v)     OBJECT_TO_JSID(JSVAL_TO_OBJECT(v))

/* Scalar typedefs. */
typedef uint8  jsbytecode;
typedef uint8  jssrcnote;
typedef uint32 jsatomid;

/* Struct typedefs. */
typedef struct JSArgumentFormatMap  JSArgumentFormatMap;
typedef struct JSCodeGenerator      JSCodeGenerator;
typedef struct JSDependentString    JSDependentString;
typedef struct JSGCThing            JSGCThing;
typedef struct JSGenerator          JSGenerator;
typedef struct JSParseNode          JSParseNode;
typedef struct JSSharpObjectMap     JSSharpObjectMap;
typedef struct JSThread             JSThread;
typedef struct JSToken              JSToken;
typedef struct JSTokenPos           JSTokenPos;
typedef struct JSTokenPtr           JSTokenPtr;
typedef struct JSTokenStream        JSTokenStream;
typedef struct JSTreeContext        JSTreeContext;
typedef struct JSTryNote            JSTryNote;

/* Friend "Advanced API" typedefs. */
typedef struct JSAtom               JSAtom;
typedef struct JSAtomList           JSAtomList;
typedef struct JSAtomListElement    JSAtomListElement;
typedef struct JSAtomMap            JSAtomMap;
typedef struct JSAtomState          JSAtomState;
typedef struct JSCodeSpec           JSCodeSpec;
typedef struct JSPrinter            JSPrinter;
typedef struct JSRegExp             JSRegExp;
typedef struct JSRegExpStatics      JSRegExpStatics;
typedef struct JSScope              JSScope;
typedef struct JSScopeOps           JSScopeOps;
typedef struct JSScopeProperty      JSScopeProperty;
typedef struct JSStackHeader        JSStackHeader;
typedef struct JSStringBuffer       JSStringBuffer;
typedef struct JSSubString          JSSubString;
typedef struct JSXML                JSXML;
typedef struct JSXMLNamespace       JSXMLNamespace;
typedef struct JSXMLQName           JSXMLQName;
typedef struct JSXMLArray           JSXMLArray;
typedef struct JSXMLArrayCursor     JSXMLArrayCursor;

/* "Friend" types used by jscntxt.h and jsdbgapi.h. */
typedef enum JSTrapStatus {
    JSTRAP_ERROR,
    JSTRAP_CONTINUE,
    JSTRAP_RETURN,
    JSTRAP_THROW,
    JSTRAP_LIMIT
} JSTrapStatus;

typedef JSTrapStatus
(* JS_DLL_CALLBACK JSTrapHandler)(JSContext *cx, JSScript *script,
                                  jsbytecode *pc, jsval *rval, void *closure);

typedef JSBool
(* JS_DLL_CALLBACK JSWatchPointHandler)(JSContext *cx, JSObject *obj, jsval id,
                                        jsval old, jsval *newp, void *closure);

/* called just after script creation */
typedef void
(* JS_DLL_CALLBACK JSNewScriptHook)(JSContext  *cx,
                                    const char *filename,  /* URL of script */
                                    uintN      lineno,     /* first line */
                                    JSScript   *script,
                                    JSFunction *fun,
                                    void       *callerdata);

/* called just before script destruction */
typedef void
(* JS_DLL_CALLBACK JSDestroyScriptHook)(JSContext *cx,
                                        JSScript  *script,
                                        void      *callerdata);

typedef void
(* JS_DLL_CALLBACK JSSourceHandler)(const char *filename, uintN lineno,
                                    jschar *str, size_t length,
                                    void **listenerTSData, void *closure);

/*
 * This hook captures high level script execution and function calls (JS or
 * native).  It is used by JS_SetExecuteHook to hook top level scripts and by
 * JS_SetCallHook to hook function calls.  It will get called twice per script
 * or function call: just before execution begins and just after it finishes.
 * In both cases the 'current' frame is that of the executing code.
 *
 * The 'before' param is JS_TRUE for the hook invocation before the execution
 * and JS_FALSE for the invocation after the code has run.
 *
 * The 'ok' param is significant only on the post execution invocation to
 * signify whether or not the code completed 'normally'.
 *
 * The 'closure' param is as passed to JS_SetExecuteHook or JS_SetCallHook
 * for the 'before'invocation, but is whatever value is returned from that
 * invocation for the 'after' invocation. Thus, the hook implementor *could*
 * allocate a structure in the 'before' invocation and return a pointer to that
 * structure. The pointer would then be handed to the hook for the 'after'
 * invocation. Alternately, the 'before' could just return the same value as
 * in 'closure' to cause the 'after' invocation to be called with the same
 * 'closure' value as the 'before'.
 *
 * Returning NULL in the 'before' hook will cause the 'after' hook *not* to
 * be called.
 */
typedef void *
(* JS_DLL_CALLBACK JSInterpreterHook)(JSContext *cx, JSStackFrame *fp, JSBool before,
                                      JSBool *ok, void *closure);

typedef void
(* JS_DLL_CALLBACK JSObjectHook)(JSContext *cx, JSObject *obj, JSBool isNew,
                                 void *closure);

typedef JSBool
(* JS_DLL_CALLBACK JSDebugErrorHook)(JSContext *cx, const char *message,
                                     JSErrorReport *report, void *closure);

#endif /* jsprvtd_h___ */
