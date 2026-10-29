// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#define MONGO_MOD_PUBLIC MONGO_MOD_ATTR_(public)

/**
 * Marks a class as open and everything inside as public to other modules.
 * This is "more public than public" in that it also allows inheriting from the class.
 * The naming as "open" and distinction from public are both inspired by Swift's access control
 * levels.
 *
 * If we later decide we do not like this distinction, we can easily just sed this macro away
 * and use MONGO_MOD_PUBLIC everywhere. But it will be easier to start with the distinction than to
 * add it later.
 *
 * NOTE: unlike other markers openness is *not* applied recursively, so each type that should
 * support external subclasses must be separately marked as open.
 */
#define MONGO_MOD_OPEN MONGO_MOD_ATTR_(open)

/**
 * Similar to MONGO_MOD_OPEN and should only be used when inheriting from a class is only allowed
 * within its module boundaries, but there are "unfortunately" existing extensions in other modules.
 *
 * For now this just serves as greppable documentation and behaves the same as OPEN.
 */
#define MONGO_MOD_UNFORTUNATELY_OPEN MONGO_MOD_ATTR_(open)

/**
 * Marks a declaration which the module owner would prefer to be
 * private, but for which there is currently no good alternative
 * for out-of-module usage. Allows external usage like MONGO_MOD_PUBLIC,
 * but gives a warning to the module owner.
 */
#define MONGO_MOD_NEEDS_REPLACEMENT MONGO_MOD_ATTR_(needs_replacement)
/**
 * Marks a declaration which the module owner would prefer to be
 * private, but for which there are historical uses outside the
 * module which haven't been cleaned up yet. Allows external usage
 * like MONGO_MOD_PUBLIC, but gives a warning to each caller for usage
 * outside the module.
 *
 * The replacement is free-form text. Usually you can just put an API in there,
 * like Foo::bar(), but if you want to write a message to the reader, you can.
 * Since [[FOO(space separated words)]] sometimes makes clang-format think the
 * file is objective-c, you can also use [[FOO("space separated words")]]. The
 * scanner will strip the quotes and treat them the same.
 */
#define MONGO_MOD_USE_REPLACEMENT(replacement) MONGO_MOD_ATTR_(use_replacement::replacement)

/**
 * Marks a declaration and everything inside it public. Should only be used for things that should
 * be private, but can't be marked so due to limitations with the scanner.
 * For example, declarations that should only be used via public macros
 * currently need to be public, even if they aren't directly used externally.
 * In almost all cases, MONGO_MOD_NEEDS_REPLACEMENT should be used instead.
 */
#define MONGO_MOD_PUBLIC_FOR_TECHNICAL_REASONS MONGO_MOD_ATTR_(public)

/** Marks a declaration and everything inside as private from other modules */
#define MONGO_MOD_PRIVATE MONGO_MOD_ATTR_(private)

/** Stronger form of private, restricts usage to the same family of h, cpp, and test.cpp files. */
#define MONGO_MOD_FILE_PRIVATE MONGO_MOD_ATTR_(file_private)

/**
 * Marks a declaration and everything inside as private from other modules, but
 * behaves as-if attached to the parent module for the purposes of who can use
 * it. For example a PARENT_PRIVATE class in module foo.bar can be used by any
 * code in modules foo, foo.bar, and foo.baz, but not by code in module qux.
 *
 * This is intended for cases where a module is split into submodules that each
 * have their own public and private APIs, but some submodules still want to
 * present APIs only to the parent module. Without this option, you would have
 * to choose between keeping the code in the parent module without the ability
 * to have its own private API, or making it public to all modules.
 *
 * This can only be used from a submodule (a module with a dot in its name).
 *
 * Note: this is defined to make the declaration visibile to the *direct* parent.
 * Because we currently only have one level of submodules, this is equivalent to
 * making it visible to the whole top-level module. If we ever have multiple levels
 * of submodules (eg query.opt.parser), we may want to allow specifying how far up
 * the hierarchy the declaration should be visible.
 */
#define MONGO_MOD_PARENT_PRIVATE MONGO_MOD_ATTR_(parent_private)

//
// Implementation details for MONGO_MOD macros
//

#if MONGO_COMPILER_HAS_ATTRIBUTE(clang::annotate)
#define MONGO_MOD_ATTR_(attr) clang::annotate("mongo::mod::" #attr)
#else
#define MONGO_MOD_ATTR_(attr)
#endif
