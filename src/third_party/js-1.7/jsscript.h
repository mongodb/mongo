/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
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

#ifndef jsscript_h___
#define jsscript_h___
/*
 * JS script descriptor.
 */
#include "jsatom.h"
#include "jsprvtd.h"

JS_BEGIN_EXTERN_C

/*
 * Exception handling runtime information.
 *
 * All fields except length are code offsets relative to the main entry point
 * of the script.  If script->trynotes is not null, it points to a vector of
 * these structs terminated by one with catchStart == 0.
 */
struct JSTryNote {
    ptrdiff_t    start;         /* start of try statement */
    ptrdiff_t    length;        /* count of try statement bytecodes */
    ptrdiff_t    catchStart;    /* start of catch block (0 if end) */
};

#define JSTRYNOTE_GRAIN         sizeof(ptrdiff_t)
#define JSTRYNOTE_ALIGNMASK     (JSTRYNOTE_GRAIN - 1)

struct JSScript {
    jsbytecode   *code;         /* bytecodes and their immediate operands */
    uint32       length;        /* length of code vector */
    jsbytecode   *main;         /* main entry point, after predef'ing prolog */
    uint16       version;       /* JS version under which script was compiled */
    uint16       numGlobalVars; /* declared global var/const/function count */
    JSAtomMap    atomMap;       /* maps immediate index to literal struct */
    const char   *filename;     /* source filename or null */
    uintN        lineno;        /* base line number of script */
    uintN        depth;         /* maximum stack depth in slots */
    JSTryNote    *trynotes;     /* exception table for this script */
    JSPrincipals *principals;   /* principals for this script */
    JSObject     *object;       /* optional Script-class object wrapper */
};

/* No need to store script->notes now that it is allocated right after code. */
#define SCRIPT_NOTES(script)    ((jssrcnote*)((script)->code+(script)->length))

#define SCRIPT_FIND_CATCH_START(script, pc, catchpc)                          \
    JS_BEGIN_MACRO                                                            \
        JSTryNote *tn_ = (script)->trynotes;                                  \
        jsbytecode *catchpc_ = NULL;                                          \
        if (tn_) {                                                            \
            ptrdiff_t off_ = PTRDIFF(pc, (script)->main, jsbytecode);         \
            if (off_ >= 0) {                                                  \
                while ((jsuword)(off_ - tn_->start) >= (jsuword)tn_->length)  \
                    ++tn_;                                                    \
                if (tn_->catchStart)                                          \
                    catchpc_ = (script)->main + tn_->catchStart;              \
            }                                                                 \
        }                                                                     \
        catchpc = catchpc_;                                                   \
    JS_END_MACRO

/*
 * Find the innermost finally block that handles the given pc. This is a
 * version of SCRIPT_FIND_CATCH_START that ignore catch blocks and is used
 * to implement generator.close().
 */
jsbytecode *
js_FindFinallyHandler(JSScript *script, jsbytecode *pc);

extern JS_FRIEND_DATA(JSClass) js_ScriptClass;

extern JSObject *
js_InitScriptClass(JSContext *cx, JSObject *obj);

/*
 * On first new context in rt, initialize script runtime state, specifically
 * the script filename table and its lock.
 */
extern JSBool
js_InitRuntimeScriptState(JSRuntime *rt);

/*
 * On last context destroy for rt, if script filenames are all GC'd, free the
 * script filename table and its lock.
 */
extern void
js_FinishRuntimeScriptState(JSRuntime *rt);

/*
 * On JS_DestroyRuntime(rt), forcibly free script filename prefixes and any
 * script filename table entries that have not been GC'd, the latter using
 * js_FinishRuntimeScriptState.
 *
 * This allows script filename prefixes to outlive any context in rt.
 */
extern void
js_FreeRuntimeScriptState(JSRuntime *rt);

extern const char *
js_SaveScriptFilename(JSContext *cx, const char *filename);

extern const char *
js_SaveScriptFilenameRT(JSRuntime *rt, const char *filename, uint32 flags);

extern uint32
js_GetScriptFilenameFlags(const char *filename);

extern void
js_MarkScriptFilename(const char *filename);

extern void
js_MarkScriptFilenames(JSRuntime *rt, JSBool keepAtoms);

extern void
js_SweepScriptFilenames(JSRuntime *rt);

/*
 * Two successively less primitive ways to make a new JSScript.  The first
 * does *not* call a non-null cx->runtime->newScriptHook -- only the second,
 * js_NewScriptFromCG, calls this optional debugger hook.
 *
 * The js_NewScript function can't know whether the script it creates belongs
 * to a function, or is top-level or eval code, but the debugger wants access
 * to the newly made script's function, if any -- so callers of js_NewScript
 * are responsible for notifying the debugger after successfully creating any
 * kind (function or other) of new JSScript.
 */
extern JSScript *
js_NewScript(JSContext *cx, uint32 length, uint32 snlength, uint32 tnlength);

extern JS_FRIEND_API(JSScript *)
js_NewScriptFromCG(JSContext *cx, JSCodeGenerator *cg, JSFunction *fun);

/*
 * New-script-hook calling is factored from js_NewScriptFromCG so that it
 * and callers of js_XDRScript can share this code.  In the case of callers
 * of js_XDRScript, the hook should be invoked only after successful decode
 * of any owning function (the fun parameter) or script object (null fun).
 */
extern JS_FRIEND_API(void)
js_CallNewScriptHook(JSContext *cx, JSScript *script, JSFunction *fun);

extern JS_FRIEND_API(void)
js_CallDestroyScriptHook(JSContext *cx, JSScript *script);

extern void
js_DestroyScript(JSContext *cx, JSScript *script);

extern void
js_MarkScript(JSContext *cx, JSScript *script);

/*
 * To perturb as little code as possible, we introduce a js_GetSrcNote lookup
 * cache without adding an explicit cx parameter.  Thus js_GetSrcNote becomes
 * a macro that uses cx from its calls' lexical environments.
 */
#define js_GetSrcNote(script,pc) js_GetSrcNoteCached(cx, script, pc)

extern jssrcnote *
js_GetSrcNoteCached(JSContext *cx, JSScript *script, jsbytecode *pc);

/* XXX need cx to lock function objects declared by prolog bytecodes. */
extern uintN
js_PCToLineNumber(JSContext *cx, JSScript *script, jsbytecode *pc);

extern jsbytecode *
js_LineNumberToPC(JSScript *script, uintN lineno);

extern JS_FRIEND_API(uintN)
js_GetScriptLineExtent(JSScript *script);

/*
 * If magic is non-null, js_XDRScript succeeds on magic number mismatch but
 * returns false in *magic; it reflects a match via a true *magic out param.
 * If magic is null, js_XDRScript returns false on bad magic number errors,
 * which it reports.
 *
 * NB: callers must call js_CallNewScriptHook after successful JSXDR_DECODE
 * and subsequent set-up of owning function or script object, if any.
 */
extern JSBool
js_XDRScript(JSXDRState *xdr, JSScript **scriptp, JSBool *magic);

JS_END_EXTERN_C

#endif /* jsscript_h___ */
