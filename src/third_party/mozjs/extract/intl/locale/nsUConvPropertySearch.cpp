/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsUConvPropertySearch.h"
#include "nsCRT.h"
#include "nsString.h"
#include "mozilla/BinarySearch.h"

namespace {

struct PropertyComparator {
  const nsCString& mKey;
  explicit PropertyComparator(const nsCString& aKey) : mKey(aKey) {}
  int operator()(const nsUConvProp& aProperty) const {
    return mKey.Compare(aProperty.mKey);
  }
};

}  // namespace

// static
nsresult nsUConvPropertySearch::SearchPropertyValue(
    const nsUConvProp aProperties[], int32_t aNumberOfProperties,
    const nsACString& aKey, nsACString& aValue) {
  using mozilla::BinarySearchIf;

  const nsCString& flat = PromiseFlatCString(aKey);
  size_t index;
  if (BinarySearchIf(aProperties, 0, aNumberOfProperties,
                     PropertyComparator(flat), &index)) {
    nsDependentCString val(aProperties[index].mValue,
                           aProperties[index].mValueLength);
    aValue.Assign(val);
    return NS_OK;
  }

  aValue.Truncate();
  return NS_ERROR_FAILURE;
}
