/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_SelfHostedIter_h
#define frontend_SelfHostedIter_h

namespace js::frontend {

// `for-of`, `for-await-of`, and spread operations are allowed on
// self-hosted JS code only when the operand is explicitly marked with
// `allowContentIter()`.
//
// This value is effectful only when emitting self-hosted JS code.
enum class SelfHostedIter {
  // The operand is not marked.
  // Also means "don't care" for non-self-hosted JS case.
  Deny,

  // The operand is marked.
  AllowContent,

  // The operand is marked and the `@@iterator` method is on the stack.
  AllowContentWith,

  // The operand is marked and the `next` method is on the stack.
  AllowContentWithNext,
};

} /* namespace js::frontend */

#endif /* frontend_SelfHostedIter_h */
