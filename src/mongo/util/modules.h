/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/platform/compiler.h"

/**
 * This header contains macros used to mark the public API of a module.
 * Including this header marks the including header as being completely marked,
 * which makes all declarations in that header default to private unless they
 * are explicitly made public. In the rare case where some declarations need to
 * be marked, but you do not want to mark the header as being completely marked,
 * include "mongo/util/modules_incompletely_marked_header.h" instead.
 *
 * If you want to mark everything in a header as private, just include this header.
 * TODO Once all headers are completely marked, remove this paragraph (including
 * the pragma) and grep for all files that include this file without using any
 * MONGO_MOD macro directly and remove the inclusion.
 */
// IWYU pragma: always_keep (see above)

/**
 * Marks a declaration and everything inside as public to other modules.
 * This allows using the declaration outside the module except as a base class. This means that by
 * default all hierarchies are closed within their module unless explicitly marked as open.
 * See MONGO_MOD_OPEN below.
 */
#define MONGO_MOD_PUB MONGO_MOD_ATTR_(public)

/**
 * Marks a class as open and everything inside as public to other modules.
 * This is "more public than public" in that it also allows inheriting from the class.
 * The naming as "open" and distinction from public are both inspired by Swift's access control
 * levels.
 *
 * If we later decide we do not like this distinction, we can easily just sed this macro away
 * and use MONGO_MOD_PUB everywhere. But it will be easier to start with the distinction than to add
 * it later.
 *
 * NOTE: unlike other markers openness is *not* applied recursively, so each type that should
 * support external subclasses must be separately marked as open.
 */
#define MONGO_MOD_OPEN MONGO_MOD_ATTR_(open)

/**
 * Marks a declaration which the module owner would prefer to be
 * private, but for which there is currently no good alternative
 * for out-of-module usage. Allows external usage like MONGO_MOD_PUB,
 * but gives a warning to the module owner.
 */
#define MONGO_MOD_NEEDS_REPLACEMENT MONGO_MOD_ATTR_(needs_replacement)
/**
 * Marks a declaration which the module owner would prefer to be
 * private, but for which there are historical uses outside the
 * module which haven't been cleaned up yet. Allows external usage
 * like MONGO_MOD_PUB, but gives a warning to each caller for usage
 * outside the module.
 */
#define MONGO_MOD_USE_REPLACEMENT(replacement) MONGO_MOD_ATTR_(use_replacement::replacement)

/**
 * Marks a declaration and everything inside it public. Should only be used for things that should
 * be private, but can't be marked so due to limitations with the scanner.
 * For example, declarations that should only be used via public macros
 * currently need to be public, even if they aren't directly used externally.
 * In almost all cases, MONGO_MOD_NEEDS_REPLACEMENT should be used instead.
 */
#define MONGO_MOD_PUB_FOR_TECHNICAL_REASONS MONGO_MOD_ATTR_(public)

/** Marks a declaration and everything inside as private from other modules */
#define MONGO_MOD_PRIVATE MONGO_MOD_ATTR_(private)

/** Stronger form of private, restricts usage to the same family of h, cpp, and test.cpp files. */
#define MONGO_MOD_FILE_PRIVATE MONGO_MOD_ATTR_(file_private)

//
// Implementation details for MONGO_MOD macros
//

#if MONGO_COMPILER_HAS_ATTRIBUTE(clang::annotate)
#define MONGO_MOD_ATTR_(attr) [[clang::annotate("mongo::mod::" #attr)]]
#else
#define MONGO_MOD_ATTR_(attr)
#endif
