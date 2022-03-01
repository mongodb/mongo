/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "mozilla/intl/MozLocale.h"

using namespace mozilla::intl;

TEST(Intl_Locale_Locale, Locale)
{
  Locale loc = Locale("en-US");

  ASSERT_TRUE(loc.GetLanguage().Equals("en"));
  ASSERT_TRUE(loc.GetRegion().Equals("US"));
}

TEST(Intl_Locale_Locale, AsString)
{
  Locale loc = Locale("ja-jp-windows");

  ASSERT_TRUE(loc.AsString().Equals("ja-JP-windows"));
}

TEST(Intl_Locale_Locale, GetSubTags)
{
  Locale loc = Locale("en-latn-us-macos");

  ASSERT_TRUE(loc.GetLanguage().Equals("en"));
  ASSERT_TRUE(loc.GetScript().Equals("Latn"));
  ASSERT_TRUE(loc.GetRegion().Equals("US"));

  nsTArray<nsCString> variants;
  loc.GetVariants(variants);
  ASSERT_TRUE(variants.Length() == 1);
  ASSERT_TRUE(variants[0].Equals("macos"));
}

TEST(Intl_Locale_Locale, Matches)
{
  Locale loc = Locale("en-US");

  Locale loc2 = Locale("en-GB");
  ASSERT_FALSE(loc == loc2);

  Locale loc3 = Locale("en-US");
  ASSERT_TRUE(loc == loc3);

  Locale loc4 = Locale("En_us");
  ASSERT_TRUE(loc == loc4);
}

TEST(Intl_Locale_Locale, MatchesRange)
{
  Locale loc = Locale("en-US");

  Locale loc2 = Locale("en-Latn-US");
  ASSERT_FALSE(loc == loc2);
  ASSERT_TRUE(loc.Matches(loc2, true, false));
  ASSERT_FALSE(loc.Matches(loc2, false, true));
  ASSERT_FALSE(loc.Matches(loc2, false, false));
  ASSERT_TRUE(loc.Matches(loc2, true, true));

  Locale loc3 = Locale("en");
  ASSERT_FALSE(loc == loc3);
  ASSERT_TRUE(loc.Matches(loc3, false, true));
  ASSERT_FALSE(loc.Matches(loc3, true, false));
  ASSERT_FALSE(loc.Matches(loc3, false, false));
  ASSERT_TRUE(loc.Matches(loc3, true, true));
}

TEST(Intl_Locale_Locale, Variants)
{
  Locale loc = Locale("en-US-UniFon-BasicEng");

  // Make sure that we canonicalize and sort variant tags
  ASSERT_TRUE(loc.AsString().Equals("en-US-basiceng-unifon"));
}

TEST(Intl_Locale_Locale, InvalidLocale)
{
  Locale loc = Locale("en-verylongsubtag");
  ASSERT_FALSE(loc.IsWellFormed());

  Locale loc2 = Locale("p-te");
  ASSERT_FALSE(loc2.IsWellFormed());
}

TEST(Intl_Locale_Locale, ClearRegion)
{
  Locale loc = Locale("en-US");
  loc.ClearRegion();
  ASSERT_TRUE(loc.AsString().Equals("en"));
}

TEST(Intl_Locale_Locale, ClearVariants)
{
  Locale loc = Locale("en-US-windows");
  loc.ClearVariants();
  ASSERT_TRUE(loc.AsString().Equals("en-US"));
}

TEST(Intl_Locale_Locale, jaJPmac)
{
  Locale loc = Locale("ja-JP-mac");
  ASSERT_TRUE(loc.AsString().Equals("ja-JP-macos"));
}

TEST(Intl_Locale_Locale, Maximize)
{
  Locale loc = Locale("en");

  ASSERT_TRUE(loc.GetLanguage().Equals("en"));
  ASSERT_TRUE(loc.GetScript().IsEmpty());
  ASSERT_TRUE(loc.GetRegion().IsEmpty());

  ASSERT_TRUE(loc.Maximize());

  ASSERT_TRUE(loc.GetLanguage().Equals("en"));
  ASSERT_TRUE(loc.GetScript().Equals("Latn"));
  ASSERT_TRUE(loc.GetRegion().Equals("US"));
}
