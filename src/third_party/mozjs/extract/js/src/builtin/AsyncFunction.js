/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

function AsyncFunctionNext(val) {
  assert(
    IsAsyncFunctionGeneratorObject(this),
    "ThisArgument must be a generator object for async functions"
  );
  return resumeGenerator(this, val, "next");
}

function AsyncFunctionThrow(val) {
  assert(
    IsAsyncFunctionGeneratorObject(this),
    "ThisArgument must be a generator object for async functions"
  );
  return resumeGenerator(this, val, "throw");
}
