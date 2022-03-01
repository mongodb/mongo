/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "nsCollationCID.h"
#include "nsComponentManagerUtils.h"
#include "nsCOMPtr.h"
#include "nsICollation.h"
#include "nsString.h"
#include "nsTArray.h"

TEST(Collation, AllocateRowSortKey)
{
  nsCOMPtr<nsICollationFactory> colFactory =
      do_CreateInstance(NS_COLLATIONFACTORY_CONTRACTID);
  ASSERT_TRUE(colFactory);

  // Don't throw error even if locale name is invalid
  nsCOMPtr<nsICollation> collator;
  nsresult rv = colFactory->CreateCollationForLocale("$languageName"_ns,
                                                     getter_AddRefs(collator));
  ASSERT_TRUE(NS_SUCCEEDED(rv));

  nsTArray<uint8_t> sortKey1;
  // Don't throw error even if locale name is invalid
  rv = collator->AllocateRawSortKey(nsICollation::kCollationStrengthDefault,
                                    u"ABC"_ns, sortKey1);
  ASSERT_TRUE(NS_SUCCEEDED(rv));

  nsTArray<uint8_t> sortKey2;
  // Don't throw error even if locale name is invalid
  rv = collator->AllocateRawSortKey(nsICollation::kCollationStrengthDefault,
                                    u"DEF"_ns, sortKey2);
  ASSERT_TRUE(NS_SUCCEEDED(rv));

  int32_t result;
  rv = collator->CompareRawSortKey(sortKey1, sortKey2, &result);
  ASSERT_TRUE(NS_SUCCEEDED(rv));

  ASSERT_TRUE(result < 0);
}

class CollationComparator final {
 public:
  explicit CollationComparator(nsICollation* aCollation)
      : mCollation(aCollation) {}

  bool Equals(const nsString& a, const nsString& b) const {
    int32_t result = 0;
    mCollation->CompareString(nsICollation::kCollationStrengthDefault, a, b,
                              &result);
    return result == 0;
  }

  bool LessThan(const nsString& a, const nsString& b) const {
    int32_t result = 0;
    mCollation->CompareString(nsICollation::kCollationStrengthDefault, a, b,
                              &result);
    return result < 0;
  }

 private:
  nsCOMPtr<nsICollation> mCollation;
};

TEST(Collation, CompareString)
{
  nsTArray<nsString> input;
  input.AppendElement(u"Argentina"_ns);
  input.AppendElement(u"Oerlikon"_ns);
  input.AppendElement(u"Offenbach"_ns);
  input.AppendElement(u"Sverige"_ns);
  input.AppendElement(u"Vaticano"_ns);
  input.AppendElement(u"Zimbabwe"_ns);
  input.AppendElement(u"la France"_ns);
  input.AppendElement(u"\u00a1viva Espa\u00f1a!"_ns);
  input.AppendElement(u"\u00d6sterreich"_ns);
  input.AppendElement(u"\u4e2d\u56fd"_ns);
  input.AppendElement(u"\u65e5\u672c"_ns);
  input.AppendElement(u"\ud55c\uad6d"_ns);

  nsCOMPtr<nsICollationFactory> colFactory =
      do_CreateInstance(NS_COLLATIONFACTORY_CONTRACTID);
  ASSERT_TRUE(colFactory);

  // Locale en-US; default options.
  nsCOMPtr<nsICollation> collation;
  colFactory->CreateCollationForLocale("en-US"_ns, getter_AddRefs(collation));
  ASSERT_TRUE(collation);

  {
    CollationComparator comparator(collation);
    input.Sort(comparator);

    ASSERT_TRUE(input[0].Equals(u"\u00a1viva Espa\u00f1a!"_ns));
    ASSERT_TRUE(input[1].Equals(u"Argentina"_ns));
    ASSERT_TRUE(input[2].Equals(u"la France"_ns));
    ASSERT_TRUE(input[3].Equals(u"Oerlikon"_ns));
    ASSERT_TRUE(input[4].Equals(u"Offenbach"_ns));
    ASSERT_TRUE(input[5].Equals(u"\u00d6sterreich"_ns));
    ASSERT_TRUE(input[6].Equals(u"Sverige"_ns));
    ASSERT_TRUE(input[7].Equals(u"Vaticano"_ns));
    ASSERT_TRUE(input[8].Equals(u"Zimbabwe"_ns));
    ASSERT_TRUE(input[9].Equals(u"\ud55c\uad6d"_ns));
    ASSERT_TRUE(input[10].Equals(u"\u4e2d\u56fd"_ns));
    ASSERT_TRUE(input[11].Equals(u"\u65e5\u672c"_ns));
  }

  // Locale sv-SE; default options.
  // Swedish treats "Ö" as a separate character, which sorts after "Z".
  colFactory->CreateCollationForLocale("sv-SE"_ns, getter_AddRefs(collation));
  ASSERT_TRUE(collation);

  {
    CollationComparator comparator(collation);
    input.Sort(comparator);

    ASSERT_TRUE(input[0].Equals(u"\u00a1viva Espa\u00f1a!"_ns));
    ASSERT_TRUE(input[1].Equals(u"Argentina"_ns));
    ASSERT_TRUE(input[2].Equals(u"la France"_ns));
    ASSERT_TRUE(input[3].Equals(u"Oerlikon"_ns));
    ASSERT_TRUE(input[4].Equals(u"Offenbach"_ns));
    ASSERT_TRUE(input[5].Equals(u"Sverige"_ns));
    ASSERT_TRUE(input[6].Equals(u"Vaticano"_ns));
    ASSERT_TRUE(input[7].Equals(u"Zimbabwe"_ns));
    ASSERT_TRUE(input[8].Equals(u"\u00d6sterreich"_ns));
    ASSERT_TRUE(input[9].Equals(u"\ud55c\uad6d"_ns));
    ASSERT_TRUE(input[10].Equals(u"\u4e2d\u56fd"_ns));
    ASSERT_TRUE(input[11].Equals(u"\u65e5\u672c"_ns));
  }

  // Locale de-DE; default options.
  // In German standard sorting, umlauted characters are treated as variants
  // of their base characters: ä ≅ a, ö ≅ o, ü ≅ u.
  colFactory->CreateCollationForLocale("de-DE"_ns, getter_AddRefs(collation));
  ASSERT_TRUE(collation);

  {
    CollationComparator comparator(collation);
    input.Sort(comparator);

    ASSERT_TRUE(input[0].Equals(u"\u00a1viva Espa\u00f1a!"_ns));
    ASSERT_TRUE(input[1].Equals(u"Argentina"_ns));
    ASSERT_TRUE(input[2].Equals(u"la France"_ns));
    ASSERT_TRUE(input[3].Equals(u"Oerlikon"_ns));
    ASSERT_TRUE(input[4].Equals(u"Offenbach"_ns));
    ASSERT_TRUE(input[5].Equals(u"\u00d6sterreich"_ns));
    ASSERT_TRUE(input[6].Equals(u"Sverige"_ns));
    ASSERT_TRUE(input[7].Equals(u"Vaticano"_ns));
    ASSERT_TRUE(input[8].Equals(u"Zimbabwe"_ns));
    ASSERT_TRUE(input[9].Equals(u"\ud55c\uad6d"_ns));
    ASSERT_TRUE(input[10].Equals(u"\u4e2d\u56fd"_ns));
    ASSERT_TRUE(input[11].Equals(u"\u65e5\u672c"_ns));
  }
}
