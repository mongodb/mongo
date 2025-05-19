/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Prefs_h
#define js_Prefs_h

#include "js/PrefsGenerated.h"

// [SMDOC] Prefs
//
// JS::Prefs is used to make JS preferences defined in StaticPrefList.yaml
// available to SpiderMonkey code.
//
// Adding a Pref
// =============
// Adding a new pref is easy. For example, if you're adding a new JS feature,
// you could add the following to StaticPrefList.yaml:
//
//   - name: javascript.options.experimental.my_new_feature
//     type: bool
//     value: false
//     mirror: always
//     set_spidermonkey_pref: startup
//
// The value of this pref can then be accessed in SpiderMonkey code with
// |JS::Prefs::experimental_my_new_feature()|.
//
// The default pref value in the YAML file applies to all SpiderMonkey builds
// (browser, JS shell, jsapi-tests, etc), so by default this feature will be
// disabled everywhere.
//
// To enable your feature, use the |--setpref experimental.my_new_feature=true|
// JS shell command line argument, or set the browser pref in about:config.
// Because this is a 'startup' pref, a browser restart is required for this to
// take effect.
//
// The rest of this comment describes more advanced use cases.
//
// Non-startup prefs
// =================
// Setting |set_spidermonkey_pref = startup| is recommended for most prefs.
// In this case the pref is only set during startup so we don't have to worry
// about the pref value changing at runtime.
//
// However, for some prefs this doesn't work. For instance, the WPT test harness
// can set test-specific prefs after startup. To properly update the JS pref in
// this case, |set_spidermonkey_pref = always| must be used. This means the
// SpiderMonkey pref will be updated whenever it's changed in the browser.
//
// Setting Prefs
// =============
// Embedders can override pref values. For startup prefs, this should only be
// done during startup (before calling JS_Init*) to avoid races with worker
// threads and to avoid confusing code with unexpected pref changes:
//
//   JS::Prefs::setAtStartup_experimental_my_new_feature(true);
//
// Non-startup prefs can also be changed after startup:
//
//   JS::Prefs::set_experimental_my_new_feature(true);
//
// JS Shell Prefs
// ==============
// The JS shell |--list-prefs| command line flag will print a list of all of the
// available JS prefs and their current values.
//
// To change a pref, use |--setpref name=value|, for example
// |--setpref experimental.my_new_feature=true|.
//
// It's also possible to add a custom shell flag. In this case you have to
// override the pref value yourself based on this flag.
//
// Testing Functions
// =================
// The |getAllPrefNames()| function will return an array with all JS pref names.
//
// The |getPrefValue(name)| function can be used to look up the value of the
// given pref. For example, use |getPrefValue("experimental.my_new_feature")|
// for the pref defined above.

namespace JS {

class Prefs {
  // For each pref, define a static |pref_| member.
  JS_PREF_CLASS_FIELDS;

#ifdef DEBUG
  static void assertCanSetStartupPref();
#else
  static void assertCanSetStartupPref() {}
#endif

 public:
  // For each pref, define static getter/setter accessors.
#define DEF_GETSET(NAME, CPP_NAME, TYPE, SETTER, IS_STARTUP_PREF) \
  static TYPE CPP_NAME() { return CPP_NAME##_; }                  \
  static void SETTER(TYPE value) {                                \
    if (IS_STARTUP_PREF) {                                        \
      assertCanSetStartupPref();                                  \
    }                                                             \
    CPP_NAME##_ = value;                                          \
  }
  FOR_EACH_JS_PREF(DEF_GETSET)
#undef DEF_GETSET

// MONGODB MODIFICATION: Define extra_gc_poisoning() explictly.
// The upstream version of MozJS defines this setting in StaticPrefList.yaml, which generates
// this symbol into PrefsGenerated.h. However, that yaml file contains ifdefs which are resolved
// during the configure step, which in the mongo repo we only run for the production variant.
// As a workaround, we mimic the behavior of the yaml/macro above by defining the function here.
// Note: the definition in StaticPrefList.yaml considers additional conditions in its ifdefs,
// however, in our embedding we only support DEBUG and JS_GC_ZEAL when building under the
// "Spidermonkey Debug" evergreen variant.
#if defined(DEBUG) || defined(JS_GC_ZEAL)
  static bool extra_gc_poisoning() {
    return true;
  }
#endif

};

/**
 * Specification for whether weak refs should be enabled and if so whether the
 * FinalizationRegistry.cleanupSome method should be present.
 */
enum class WeakRefSpecifier {
  Disabled,
  EnabledWithCleanupSome,
  EnabledWithoutCleanupSome
};

inline WeakRefSpecifier GetWeakRefsEnabled() {
  if (!Prefs::weakrefs()) {
    return WeakRefSpecifier::Disabled;
  }
  if (Prefs::experimental_weakrefs_expose_cleanupSome()) {
    return WeakRefSpecifier::EnabledWithCleanupSome;
  }
  return WeakRefSpecifier::EnabledWithoutCleanupSome;
}

};  // namespace JS

#endif /* js_Prefs_h */
