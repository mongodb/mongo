/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * javascript provider probes
 *
 * function-entry       (filename, classname, funcname)
 * function-return      (filename, classname, funcname)
 * object-create        (classname, *object)
 * object-finalize      (NULL, classname, *object)
 * execute-start        (filename, lineno)
 * execute-done         (filename, lineno)
 */

provider javascript {
 probe function__entry(const char *, const char *, const char *);
 probe function__return(const char *, const char *, const char *);
 /* XXX must use unsigned longs here instead of uintptr_t for OS X
    (Apple radar: 5194316 & 5565198) */
 probe object__create(const char *, unsigned long);
 probe object__finalize(const char *, const char *, unsigned long);
 probe execute__start(const char *, int);
 probe execute__done(const char *, int);
};

/*
#pragma D attributes Unstable/Unstable/Common provider mozilla provider
#pragma D attributes Private/Private/Unknown provider mozilla module
#pragma D attributes Private/Private/Unknown provider mozilla function
#pragma D attributes Unstable/Unstable/Common provider mozilla name
*/

