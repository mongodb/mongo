/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef util_Identifier_h
#define util_Identifier_h

#include <stddef.h>  // size_t

#include "js/TypeDecls.h"  // JS::Latin1Char

class JSLinearString;

namespace js {

/*
 * True if str consists of an IdentifierStart character, followed by one or
 * more IdentifierPart characters, i.e. it matches the IdentifierName production
 * in the language spec.
 *
 * This returns true even if str is a keyword like "if".
 */
bool IsIdentifier(JSLinearString* str);

/*
 * As above, but taking chars + length.
 */
bool IsIdentifier(const JS::Latin1Char* chars, size_t length);
bool IsIdentifier(const char16_t* chars, size_t length);

/*
 * ASCII variant with known length.
 */
bool IsIdentifierASCII(char c);
bool IsIdentifierASCII(char c1, char c2);

/*
 * True if str consists of an optional leading '#', followed by an
 * IdentifierStart character, followed by one or more IdentifierPart characters,
 * i.e. it matches the IdentifierName production or PrivateIdentifier production
 * in the language spec.
 *
 * This returns true even if str is a keyword like "if".
 */
bool IsIdentifierNameOrPrivateName(JSLinearString* str);

/*
 * As above, but taking chars + length.
 */
bool IsIdentifierNameOrPrivateName(const JS::Latin1Char* chars, size_t length);
bool IsIdentifierNameOrPrivateName(const char16_t* chars, size_t length);

} /* namespace js */

#endif /* util_Identifier_h */
