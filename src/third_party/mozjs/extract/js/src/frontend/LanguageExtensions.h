/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Code related to various SpiderMonkey-specific language syntax extensions. */

#ifndef frontend_LanguageExtensions_h
#define frontend_LanguageExtensions_h

namespace js {

/**
 * Numeric identifiers for various deprecated language extensions.
 *
 * The initializer numbers are directly used in telemetry, so while it's okay
 * to *remove* values as language extensions are removed from SpiderMonkey,
 * it's *not* okay to compact or reorder them.  When an initializer falls into
 * disuse, remove it without reassigning its value to a new or existing
 * initializer.  The *only* initializer whose value should ever change is
 * DeprecatedLanguageExtension::Count.
 */
enum class DeprecatedLanguageExtension
{
    // NO LONGER USING 0
    // NO LONGER USING 1
    // NO LONGER USING 2
    ExpressionClosure = 3, // Added in JS 1.8
    // NO LONGER USING 4
    // NO LONGER USING 5
    // NO LONGER USING 6
    // NO LONGER USING 7
    // NO LONGER USING 8
    // NO LONGER USING 9
    // NO LONGER USING 10

    // Sentinel value.  MAY change as extension initializers are added (only to
    // the end) above.
    Count
};

} // namespace js

#endif /* frontend_LanguageExtensions_h */
