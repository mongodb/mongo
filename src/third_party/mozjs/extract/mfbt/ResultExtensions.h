/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Extensions to the Result type to enable simpler handling of XPCOM/NSPR results. */

#ifndef mozilla_ResultExtensions_h
#define mozilla_ResultExtensions_h

#include "mozilla/Assertions.h"
#include "nscore.h"
#include "prtypes.h"

namespace mozilla {

// Allow nsresult errors to automatically convert to nsresult values, so MOZ_TRY
// can be used in XPCOM methods with Result<T, nserror> results.
template <>
class MOZ_MUST_USE_TYPE GenericErrorResult<nsresult>
{
  nsresult mErrorValue;

  template<typename V, typename E2> friend class Result;

public:
  explicit GenericErrorResult(nsresult aErrorValue) : mErrorValue(aErrorValue)
  {
    MOZ_ASSERT(NS_FAILED(aErrorValue));
  }

  operator nsresult() { return mErrorValue; }
};

// Allow MOZ_TRY to handle `PRStatus` values.
inline Result<Ok, nsresult> ToResult(PRStatus aValue);

} // namespace mozilla

#include "mozilla/Result.h"

namespace mozilla {

inline Result<Ok, nsresult>
ToResult(nsresult aValue)
{
  if (NS_FAILED(aValue)) {
    return Err(aValue);
  }
  return Ok();
}

inline Result<Ok, nsresult>
ToResult(PRStatus aValue)
{
  if (aValue == PR_SUCCESS) {
    return Ok();
  }
  return Err(NS_ERROR_FAILURE);
}

} // namespace mozilla

#endif // mozilla_ResultExtensions_h
