#!/usr/bin/env python
# -*- coding: utf-8 -*-
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

""" Usage:
    make_intl_data.py langtags [cldr_common.zip]
    make_intl_data.py tzdata
    make_intl_data.py currency
    make_intl_data.py units
    make_intl_data.py numbering


    Target "langtags":
    This script extracts information about 1) mappings between deprecated and
    current Unicode BCP 47 locale identifiers, and 2) deprecated and current
    BCP 47 Unicode extension value from CLDR, and converts it to C++ mapping
    code in intl/components/LocaleGenerated.cpp. The code is used in
    intl/components/Locale.cpp.


    Target "tzdata":
    This script computes which time zone informations are not up-to-date in ICU
    and provides the necessary mappings to workaround this problem.
    https://ssl.icu-project.org/trac/ticket/12044


    Target "currency":
    Generates the mapping from currency codes to decimal digits used for them.


    Target "units":
    Generate source and test files using the list of so-called "sanctioned unit
    identifiers" and verifies that the ICU data filter includes these units.


    Target "numbering":
    Generate source and test files using the list of numbering systems with
    simple digit mappings and verifies that it's in sync with ICU/CLDR.
"""

import io
import json
import os
import re
import sys
import tarfile
import tempfile
from contextlib import closing
from functools import partial, total_ordering
from itertools import chain, groupby, tee
from operator import attrgetter, itemgetter
from zipfile import ZipFile

import yaml

if sys.version_info.major == 2:
    from itertools import ifilter as filter
    from itertools import ifilterfalse as filterfalse
    from itertools import imap as map
    from itertools import izip_longest as zip_longest

    from urllib2 import Request as UrlRequest
    from urllib2 import urlopen
    from urlparse import urlsplit
else:
    from itertools import filterfalse, zip_longest
    from urllib.parse import urlsplit
    from urllib.request import Request as UrlRequest
    from urllib.request import urlopen


# From https://docs.python.org/3/library/itertools.html
def grouper(iterable, n, fillvalue=None):
    "Collect data into fixed-length chunks or blocks"
    # grouper('ABCDEFG', 3, 'x') --> ABC DEF Gxx"
    args = [iter(iterable)] * n
    return zip_longest(*args, fillvalue=fillvalue)


def writeMappingHeader(println, description, source, url):
    if type(description) is not list:
        description = [description]
    for desc in description:
        println("// {0}".format(desc))
    println("// Derived from {0}.".format(source))
    println("// {0}".format(url))


def writeMappingsVar(println, mapping, name, description, source, url):
    """Writes a variable definition with a mapping table.

    Writes the contents of dictionary |mapping| through the |println|
    function with the given variable name and a comment with description,
    fileDate, and URL.
    """
    println("")
    writeMappingHeader(println, description, source, url)
    println("var {0} = {{".format(name))
    for key, value in sorted(mapping.items(), key=itemgetter(0)):
        println('    "{0}": "{1}",'.format(key, value))
    println("};")


def writeMappingsBinarySearch(
    println,
    fn_name,
    type_name,
    name,
    validate_fn,
    validate_case_fn,
    mappings,
    tag_maxlength,
    description,
    source,
    url,
):
    """Emit code to perform a binary search on language tag subtags.

    Uses the contents of |mapping|, which can either be a dictionary or set,
    to emit a mapping function to find subtag replacements.
    """
    println("")
    writeMappingHeader(println, description, source, url)
    println(
        """
bool mozilla::intl::Locale::{0}({1} {2}) {{
  MOZ_ASSERT({3}({2}.Span()));
  MOZ_ASSERT({4}({2}.Span()));
""".format(
            fn_name, type_name, name, validate_fn, validate_case_fn
        ).strip()
    )
    writeMappingsBinarySearchBody(println, name, name, mappings, tag_maxlength)

    println(
        """
}""".lstrip(
            "\n"
        )
    )


def writeMappingsBinarySearchBody(
    println, source_name, target_name, mappings, tag_maxlength
):
    def write_array(subtags, name, length, fixed):
        if fixed:
            println(
                "    static const char {}[{}][{}] = {{".format(
                    name, len(subtags), length + 1
                )
            )
        else:
            println("    static const char* {}[{}] = {{".format(name, len(subtags)))

        # Group in pairs of ten to not exceed the 80 line column limit.
        for entries in grouper(subtags, 10):
            entries = (
                '"{}"'.format(tag).rjust(length + 2)
                for tag in entries
                if tag is not None
            )
            println("      {},".format(", ".join(entries)))

        println("    };")

    trailing_return = True

    # Sort the subtags by length. That enables using an optimized comparator
    # for the binary search, which only performs a single |memcmp| for multiple
    # of two subtag lengths.
    mappings_keys = mappings.keys() if type(mappings) == dict else mappings
    for length, subtags in groupby(sorted(mappings_keys, key=len), len):
        # Omit the length check if the current length is the maximum length.
        if length != tag_maxlength:
            println(
                """
  if ({}.Length() == {}) {{
""".format(
                    source_name, length
                ).rstrip(
                    "\n"
                )
            )
        else:
            trailing_return = False
            println(
                """
  {
""".rstrip(
                    "\n"
                )
            )

        # The subtags need to be sorted for binary search to work.
        subtags = sorted(subtags)

        def equals(subtag):
            return """{}.EqualTo("{}")""".format(source_name, subtag)

        # Don't emit a binary search for short lists.
        if len(subtags) == 1:
            if type(mappings) == dict:
                println(
                    """
    if ({}) {{
      {}.Set(mozilla::MakeStringSpan("{}"));
      return true;
    }}
    return false;
""".format(
                        equals(subtags[0]), target_name, mappings[subtags[0]]
                    ).strip(
                        "\n"
                    )
                )
            else:
                println(
                    """
    return {};
""".format(
                        equals(subtags[0])
                    ).strip(
                        "\n"
                    )
                )
        elif len(subtags) <= 4:
            if type(mappings) == dict:
                for subtag in subtags:
                    println(
                        """
    if ({}) {{
      {}.Set("{}");
      return true;
    }}
""".format(
                            equals(subtag), target_name, mappings[subtag]
                        ).strip(
                            "\n"
                        )
                    )

                println(
                    """
    return false;
""".strip(
                        "\n"
                    )
                )
            else:
                cond = (equals(subtag) for subtag in subtags)
                cond = (" ||\n" + " " * (4 + len("return "))).join(cond)
                println(
                    """
    return {};
""".format(
                        cond
                    ).strip(
                        "\n"
                    )
                )
        else:
            write_array(subtags, source_name + "s", length, True)

            if type(mappings) == dict:
                write_array([mappings[k] for k in subtags], "aliases", length, False)

                println(
                    """
    if (const char* replacement = SearchReplacement({0}s, aliases, {0})) {{
      {1}.Set(mozilla::MakeStringSpan(replacement));
      return true;
    }}
    return false;
""".format(
                        source_name, target_name
                    ).rstrip()
                )
            else:
                println(
                    """
    return HasReplacement({0}s, {0});
""".format(
                        source_name
                    ).rstrip()
                )

        println(
            """
  }
""".strip(
                "\n"
            )
        )

    if trailing_return:
        println(
            """
  return false;"""
        )


def writeComplexLanguageTagMappings(
    println, complex_language_mappings, description, source, url
):
    println("")
    writeMappingHeader(println, description, source, url)
    println(
        """
void mozilla::intl::Locale::PerformComplexLanguageMappings() {
  MOZ_ASSERT(IsStructurallyValidLanguageTag(Language().Span()));
  MOZ_ASSERT(IsCanonicallyCasedLanguageTag(Language().Span()));
""".lstrip()
    )

    # Merge duplicate language entries.
    language_aliases = {}
    for deprecated_language, (language, script, region) in sorted(
        complex_language_mappings.items(), key=itemgetter(0)
    ):
        key = (language, script, region)
        if key not in language_aliases:
            language_aliases[key] = []
        else:
            language_aliases[key].append(deprecated_language)

    first_language = True
    for deprecated_language, (language, script, region) in sorted(
        complex_language_mappings.items(), key=itemgetter(0)
    ):
        key = (language, script, region)
        if deprecated_language in language_aliases[key]:
            continue

        if_kind = "if" if first_language else "else if"
        first_language = False

        cond = (
            'Language().EqualTo("{}")'.format(lang)
            for lang in [deprecated_language] + language_aliases[key]
        )
        cond = (" ||\n" + " " * (2 + len(if_kind) + 2)).join(cond)

        println(
            """
  {} ({}) {{""".format(
                if_kind, cond
            ).strip(
                "\n"
            )
        )

        println(
            """
    SetLanguage("{}");""".format(
                language
            ).strip(
                "\n"
            )
        )

        if script is not None:
            println(
                """
    if (Script().Missing()) {{
      SetScript("{}");
    }}""".format(
                    script
                ).strip(
                    "\n"
                )
            )
        if region is not None:
            println(
                """
    if (Region().Missing()) {{
      SetRegion("{}");
    }}""".format(
                    region
                ).strip(
                    "\n"
                )
            )
        println(
            """
  }""".strip(
                "\n"
            )
        )

    println(
        """
}
""".strip(
            "\n"
        )
    )


def writeComplexRegionTagMappings(
    println, complex_region_mappings, description, source, url
):
    println("")
    writeMappingHeader(println, description, source, url)
    println(
        """
void mozilla::intl::Locale::PerformComplexRegionMappings() {
  MOZ_ASSERT(IsStructurallyValidLanguageTag(Language().Span()));
  MOZ_ASSERT(IsCanonicallyCasedLanguageTag(Language().Span()));
  MOZ_ASSERT(IsStructurallyValidRegionTag(Region().Span()));
  MOZ_ASSERT(IsCanonicallyCasedRegionTag(Region().Span()));
""".lstrip()
    )

    # |non_default_replacements| is a list and hence not hashable. Convert it
    # to a string to get a proper hashable value.
    def hash_key(default, non_default_replacements):
        return (default, str(sorted(str(v) for v in non_default_replacements)))

    # Merge duplicate region entries.
    region_aliases = {}
    for deprecated_region, (default, non_default_replacements) in sorted(
        complex_region_mappings.items(), key=itemgetter(0)
    ):
        key = hash_key(default, non_default_replacements)
        if key not in region_aliases:
            region_aliases[key] = []
        else:
            region_aliases[key].append(deprecated_region)

    first_region = True
    for deprecated_region, (default, non_default_replacements) in sorted(
        complex_region_mappings.items(), key=itemgetter(0)
    ):
        key = hash_key(default, non_default_replacements)
        if deprecated_region in region_aliases[key]:
            continue

        if_kind = "if" if first_region else "else if"
        first_region = False

        cond = (
            'Region().EqualTo("{}")'.format(region)
            for region in [deprecated_region] + region_aliases[key]
        )
        cond = (" ||\n" + " " * (2 + len(if_kind) + 2)).join(cond)

        println(
            """
  {} ({}) {{""".format(
                if_kind, cond
            ).strip(
                "\n"
            )
        )

        replacement_regions = sorted(
            {region for (_, _, region) in non_default_replacements}
        )

        first_case = True
        for replacement_region in replacement_regions:
            replacement_language_script = sorted(
                (language, script)
                for (language, script, region) in (non_default_replacements)
                if region == replacement_region
            )

            if_kind = "if" if first_case else "else if"
            first_case = False

            def compare_tags(language, script):
                if script is None:
                    return 'Language().EqualTo("{}")'.format(language)
                return '(Language().EqualTo("{}") && Script().EqualTo("{}"))'.format(
                    language, script
                )

            cond = (
                compare_tags(language, script)
                for (language, script) in replacement_language_script
            )
            cond = (" ||\n" + " " * (4 + len(if_kind) + 2)).join(cond)

            println(
                """
    {} ({}) {{
      SetRegion("{}");
    }}""".format(
                    if_kind, cond, replacement_region
                )
                .rstrip()
                .strip("\n")
            )

        println(
            """
    else {{
      SetRegion("{}");
    }}
  }}""".format(
                default
            )
            .rstrip()
            .strip("\n")
        )

    println(
        """
}
""".strip(
            "\n"
        )
    )


def writeVariantTagMappings(println, variant_mappings, description, source, url):
    """Writes a function definition that maps variant subtags."""
    println(
        """
static const char* ToCharPointer(const char* str) {
  return str;
}

static const char* ToCharPointer(const mozilla::intl::UniqueChars& str) {
  return str.get();
}

template <typename T, typename U = T>
static bool IsLessThan(const T& a, const U& b) {
  return strcmp(ToCharPointer(a), ToCharPointer(b)) < 0;
}
"""
    )
    writeMappingHeader(println, description, source, url)
    println(
        """
bool mozilla::intl::Locale::PerformVariantMappings() {
  // The variant subtags need to be sorted for binary search.
  MOZ_ASSERT(std::is_sorted(mVariants.begin(), mVariants.end(),
                            IsLessThan<decltype(mVariants)::ElementType>));

  auto removeVariantAt = [&](size_t index) {
    mVariants.erase(mVariants.begin() + index);
  };

  auto insertVariantSortedIfNotPresent = [&](const char* variant) {
    auto* p = std::lower_bound(
        mVariants.begin(), mVariants.end(), variant,
        IsLessThan<decltype(mVariants)::ElementType, decltype(variant)>);

    // Don't insert the replacement when already present.
    if (p != mVariants.end() && strcmp(p->get(), variant) == 0) {
      return true;
    }

    // Insert the preferred variant in sort order.
    auto preferred = DuplicateStringToUniqueChars(variant);
    return !!mVariants.insert(p, std::move(preferred));
  };

  for (size_t i = 0; i < mVariants.length();) {
    const char* variant = mVariants[i].get();
    MOZ_ASSERT(IsCanonicallyCasedVariantTag(mozilla::MakeStringSpan(variant)));
""".lstrip()
    )

    (no_alias, with_alias) = partition(
        variant_mappings.items(), lambda item: item[1] is None
    )

    no_replacements = " ||\n        ".join(
        f"""strcmp(variant, "{deprecated_variant}") == 0"""
        for (deprecated_variant, _) in sorted(no_alias, key=itemgetter(0))
    )

    println(
        f"""
    if ({no_replacements}) {{
      removeVariantAt(i);
    }}
""".strip(
            "\n"
        )
    )

    for deprecated_variant, (type, replacement) in sorted(
        with_alias, key=itemgetter(0)
    ):
        println(
            f"""
    else if (strcmp(variant, "{deprecated_variant}") == 0) {{
      removeVariantAt(i);
""".strip(
                "\n"
            )
        )

        if type == "language":
            println(
                f"""
      SetLanguage("{replacement}");
""".strip(
                    "\n"
                )
            )
        elif type == "region":
            println(
                f"""
      SetRegion("{replacement}");
""".strip(
                    "\n"
                )
            )
        else:
            assert type == "variant"
            println(
                f"""
      if (!insertVariantSortedIfNotPresent("{replacement}")) {{
        return false;
      }}
""".strip(
                    "\n"
                )
            )

        println(
            """
    }
""".strip(
                "\n"
            )
        )

    println(
        """
    else {
      i++;
    }
  }
  return true;
}
""".strip(
            "\n"
        )
    )


def writeLegacyMappingsFunction(println, legacy_mappings, description, source, url):
    """Writes a function definition that maps legacy language tags."""
    println("")
    writeMappingHeader(println, description, source, url)
    println(
        """\
bool mozilla::intl::Locale::UpdateLegacyMappings() {
  // We're mapping legacy tags to non-legacy form here.
  // Other tags remain unchanged.
  //
  // Legacy tags are either sign language tags ("sgn") or have one or multiple
  // variant subtags. Therefore we can quickly exclude most tags by checking
  // these two subtags.

  MOZ_ASSERT(IsCanonicallyCasedLanguageTag(Language().Span()));

  if (!Language().EqualTo("sgn") && mVariants.length() == 0) {
    return true;
  }

#ifdef DEBUG
  for (const auto& variant : Variants()) {
    MOZ_ASSERT(IsStructurallyValidVariantTag(variant));
    MOZ_ASSERT(IsCanonicallyCasedVariantTag(variant));
  }
#endif

  // The variant subtags need to be sorted for binary search.
  MOZ_ASSERT(std::is_sorted(mVariants.begin(), mVariants.end(),
                            IsLessThan<decltype(mVariants)::ElementType>));

  auto findVariant = [this](const char* variant) {
    auto* p = std::lower_bound(mVariants.begin(), mVariants.end(), variant,
                               IsLessThan<decltype(mVariants)::ElementType,
                                          decltype(variant)>);

    if (p != mVariants.end() && strcmp(p->get(), variant) == 0) {
      return p;
    }
    return static_cast<decltype(p)>(nullptr);
  };

  auto insertVariantSortedIfNotPresent = [&](const char* variant) {
    auto* p = std::lower_bound(mVariants.begin(), mVariants.end(), variant,
                               IsLessThan<decltype(mVariants)::ElementType,
                                          decltype(variant)>);

    // Don't insert the replacement when already present.
    if (p != mVariants.end() && strcmp(p->get(), variant) == 0) {
      return true;
    }

    // Insert the preferred variant in sort order.
    auto preferred = DuplicateStringToUniqueChars(variant);
    return !!mVariants.insert(p, std::move(preferred));
  };

  auto removeVariant = [&](auto* p) {
    size_t index = std::distance(mVariants.begin(), p);
    mVariants.erase(mVariants.begin() + index);
  };

  auto removeVariants = [&](auto* p, auto* q) {
    size_t pIndex = std::distance(mVariants.begin(), p);
    size_t qIndex = std::distance(mVariants.begin(), q);
    MOZ_ASSERT(pIndex < qIndex, "variant subtags are sorted");

    mVariants.erase(mVariants.begin() + qIndex);
    mVariants.erase(mVariants.begin() + pIndex);
  };"""
    )

    # Helper class for pattern matching.
    class AnyClass:
        def __eq__(self, obj):
            return obj is not None

    Any = AnyClass()

    # Group the mappings by language.
    legacy_mappings_by_language = {}
    for type, replacement in legacy_mappings.items():
        (language, _, _, _) = type
        legacy_mappings_by_language.setdefault(language, {})[type] = replacement

    # Handle the empty language case first.
    if None in legacy_mappings_by_language:
        # Get the mappings and remove them from the dict.
        mappings = legacy_mappings_by_language.pop(None)

        # This case only applies for the "hepburn-heploc" -> "alalc97"
        # mapping, so just inline it here.
        from_tag = (None, None, None, "hepburn-heploc")
        to_tag = (None, None, None, "alalc97")

        assert len(mappings) == 1
        assert mappings[from_tag] == to_tag

        println(
            """
  if (mVariants.length() >= 2) {
    if (auto* hepburn = findVariant("hepburn")) {
      if (auto* heploc = findVariant("heploc")) {
        removeVariants(hepburn, heploc);

        if (!insertVariantSortedIfNotPresent("alalc97")) {
          return false;
        }
      }
    }
  }
"""
        )

    # Handle sign languages next.
    if "sgn" in legacy_mappings_by_language:
        mappings = legacy_mappings_by_language.pop("sgn")

        # Legacy sign language mappings have the form "sgn-XX" where "XX" is
        # some region code.
        assert all(type == ("sgn", None, Any, None) for type in mappings.keys())

        # Legacy sign languages are mapped to a single language subtag.
        assert all(
            replacement == (Any, None, None, None) for replacement in mappings.values()
        )

        println(
            """
  if (Language().EqualTo("sgn")) {
    if (Region().Present() && SignLanguageMapping(mLanguage, Region())) {
      mRegion.Set(mozilla::MakeStringSpan(""));
    }
  }
""".rstrip().lstrip(
                "\n"
            )
        )

    # Finally handle all remaining cases.

    # The remaining mappings have neither script nor region subtags in the source locale.
    assert all(
        type == (Any, None, None, Any)
        for mappings in legacy_mappings_by_language.values()
        for type in mappings.keys()
    )

    # And they have neither script nor region nor variant subtags in the target locale.
    assert all(
        replacement == (Any, None, None, None)
        for mappings in legacy_mappings_by_language.values()
        for replacement in mappings.values()
    )

    # Compact the mappings table by removing empty fields.
    legacy_mappings_by_language = {
        lang: {
            variants: r_language
            for ((_, _, _, variants), (r_language, _, _, _)) in mappings.items()
        }
        for (lang, mappings) in legacy_mappings_by_language.items()
    }

    # Try to combine the remaining cases.
    legacy_mappings_compact = {}

    # Python can't hash dicts or lists, so use the string representation as the hash key.
    def hash_key(mappings):
        return str(sorted(mappings.items(), key=itemgetter(0)))

    for lang, mappings in sorted(
        legacy_mappings_by_language.items(), key=itemgetter(0)
    ):
        key = hash_key(mappings)
        legacy_mappings_compact.setdefault(key, []).append(lang)

    for langs in legacy_mappings_compact.values():
        language_equal_to = (
            f"""Language().EqualTo("{lang}")""" for lang in sorted(langs)
        )
        cond = f""" ||\n{" " * len("  else if (")}""".join(language_equal_to)

        println(
            f"""
  else if ({cond}) {{
""".rstrip().lstrip(
                "\n"
            )
        )

        mappings = legacy_mappings_by_language[langs[0]]

        # Count the variant subtags to determine the sort order.
        def variant_size(m):
            (k, _) = m
            return len(k.split("-"))

        # Alias rules are applied by largest union size first.
        for size, mappings_by_size in groupby(
            sorted(mappings.items(), key=variant_size, reverse=True), key=variant_size
        ):
            # Convert grouper object to dict.
            mappings_by_size = dict(mappings_by_size)

            is_first = True
            chain_if = size == 1

            # Alias rules are applied in alphabetical order
            for variants, r_language in sorted(
                mappings_by_size.items(), key=itemgetter(0)
            ):
                sorted_variants = sorted(variants.split("-"))
                len_variants = len(sorted_variants)

                maybe_else = "else " if chain_if and not is_first else ""
                is_first = False

                for i, variant in enumerate(sorted_variants):
                    println(
                        f"""
    {"  " * i}{maybe_else}if (auto* {variant} = findVariant("{variant}")) {{
""".rstrip().lstrip(
                            "\n"
                        )
                    )

                indent = "  " * len_variants

                println(
                    f"""
    {indent}removeVariant{"s" if len_variants > 1 else ""}({", ".join(sorted_variants)});
    {indent}SetLanguage("{r_language}");
    {indent}{"return true;" if not chain_if else ""}
""".rstrip().lstrip(
                        "\n"
                    )
                )

                for i in range(len_variants, 0, -1):
                    println(
                        f"""
    {"  " * (i - 1)}}}
""".rstrip().lstrip(
                            "\n"
                        )
                    )

        println(
            """
  }
""".rstrip().lstrip(
                "\n"
            )
        )

    println(
        """
  return true;
}"""
    )


def writeSignLanguageMappingsFunction(
    println, legacy_mappings, description, source, url
):
    """Writes a function definition that maps legacy sign language tags."""
    println("")
    writeMappingHeader(println, description, source, url)
    println(
        """\
bool mozilla::intl::Locale::SignLanguageMapping(LanguageSubtag& language,
                                                const RegionSubtag& region) {
  MOZ_ASSERT(language.EqualTo("sgn"));
  MOZ_ASSERT(IsStructurallyValidRegionTag(region.Span()));
  MOZ_ASSERT(IsCanonicallyCasedRegionTag(region.Span()));
""".rstrip()
    )

    region_mappings = {
        rg: lg
        for ((lang, _, rg, _), (lg, _, _, _)) in legacy_mappings.items()
        if lang == "sgn"
    }

    source_name = "region"
    target_name = "language"
    tag_maxlength = 3
    writeMappingsBinarySearchBody(
        println, source_name, target_name, region_mappings, tag_maxlength
    )

    println(
        """
}""".lstrip()
    )


def readSupplementalData(core_file):
    """Reads CLDR Supplemental Data and extracts information for Intl.js.

    Information extracted:
    - legacyMappings: mappings from legacy tags to preferred complete language tags
    - languageMappings: mappings from language subtags to preferred subtags
    - complexLanguageMappings: mappings from language subtags with complex rules
    - regionMappings: mappings from region subtags to preferred subtags
    - complexRegionMappings: mappings from region subtags with complex rules
    - variantMappings: mappings from variant subtags to preferred subtags
    - likelySubtags: likely subtags used for generating test data only
    Returns these mappings as dictionaries.
    """
    import xml.etree.ElementTree as ET

    # From Unicode BCP 47 locale identifier <https://unicode.org/reports/tr35/>.
    re_unicode_language_id = re.compile(
        r"""
        ^
        # unicode_language_id = unicode_language_subtag
        #     unicode_language_subtag = alpha{2,3} | alpha{5,8}
        (?P<language>[a-z]{2,3}|[a-z]{5,8})

        # (sep unicode_script_subtag)?
        #     unicode_script_subtag = alpha{4}
        (?:-(?P<script>[a-z]{4}))?

        # (sep unicode_region_subtag)?
        #     unicode_region_subtag = (alpha{2} | digit{3})
        (?:-(?P<region>([a-z]{2}|[0-9]{3})))?

        # (sep unicode_variant_subtag)*
        #     unicode_variant_subtag = (alphanum{5,8} | digit alphanum{3})
        (?P<variants>(-([a-z0-9]{5,8}|[0-9][a-z0-9]{3}))+)?
        $
        """,
        re.IGNORECASE | re.VERBOSE,
    )

    # CLDR uses "_" as the separator for some elements. Replace it with "-".
    def bcp47_id(cldr_id):
        return cldr_id.replace("_", "-")

    # Return the tuple (language, script, region, variants) and assert all
    # subtags are in canonical case.
    def bcp47_canonical(language, script, region, variants):
        # Canonical case for language subtags is lower case.
        assert language is None or language.lower() == language

        # Canonical case for script subtags is title case.
        assert script is None or script.title() == script

        # Canonical case for region subtags is upper case.
        assert region is None or region.upper() == region

        # Canonical case for variant subtags is lower case.
        assert variants is None or variants.lower() == variants

        return (language, script, region, variants[1:] if variants else None)

    # Language ids are interpreted as multi-maps in
    # <https://www.unicode.org/reports/tr35/#LocaleId_Canonicalization>.
    #
    # See UTS35, §Annex C, Definitions - 1. Multimap interpretation.
    def language_id_to_multimap(language_id):
        match = re_unicode_language_id.match(language_id)
        assert (
            match is not None
        ), f"{language_id} invalid Unicode BCP 47 locale identifier"

        canonical_language_id = bcp47_canonical(
            *match.group("language", "script", "region", "variants")
        )
        (language, _, _, _) = canonical_language_id

        # Normalize "und" language to None, but keep the rest as is.
        return (language if language != "und" else None,) + canonical_language_id[1:]

    rules = {}
    territory_exception_rules = {}

    tree = ET.parse(core_file.open("common/supplemental/supplementalMetadata.xml"))

    # Load the rules from supplementalMetadata.xml.
    #
    # See UTS35, §Annex C, Definitions - 2. Alias elements.
    # See UTS35, §Annex C, Preprocessing.
    for alias_name in [
        "languageAlias",
        "scriptAlias",
        "territoryAlias",
        "variantAlias",
    ]:
        for alias in tree.iterfind(".//" + alias_name):
            # Replace '_' by '-'.
            type = bcp47_id(alias.get("type"))
            replacement = bcp47_id(alias.get("replacement"))

            # Prefix with "und-".
            if alias_name != "languageAlias":
                type = "und-" + type

            # Discard all rules where the type is an invalid languageId.
            if re_unicode_language_id.match(type) is None:
                continue

            type = language_id_to_multimap(type)

            # Multiple, whitespace-separated territory replacements may be present.
            if alias_name == "territoryAlias" and " " in replacement:
                replacements = replacement.split(" ")
                replacement_list = [
                    language_id_to_multimap("und-" + r) for r in replacements
                ]

                assert (
                    type not in territory_exception_rules
                ), f"Duplicate alias rule: {type}"

                territory_exception_rules[type] = replacement_list

                # The first element is the default territory replacement.
                replacement = replacements[0]

            # Prefix with "und-".
            if alias_name != "languageAlias":
                replacement = "und-" + replacement

            replacement = language_id_to_multimap(replacement)

            assert type not in rules, f"Duplicate alias rule: {type}"

            rules[type] = replacement

    # Helper class for pattern matching.
    class AnyClass:
        def __eq__(self, obj):
            return obj is not None

    Any = AnyClass()

    modified_rules = True
    loop_count = 0

    while modified_rules:
        modified_rules = False
        loop_count += 1

        # UTS 35 defines that canonicalization is applied until a fixed point has
        # been reached. This iterative application of the canonicalization algorithm
        # is only needed for a relatively small set of rules, so we can precompute
        # the transitive closure of all rules here and then perform a single pass
        # when canonicalizing language tags at runtime.
        transitive_rules = {}

        # Compute the transitive closure.
        # Any case which currently doesn't occur in the CLDR sources isn't supported
        # and will lead to throwing an error.
        for type, replacement in rules.items():
            (language, script, region, variants) = type
            (r_language, r_script, r_region, r_variants) = replacement

            for i_type, i_replacement in rules.items():
                (i_language, i_script, i_region, i_variants) = i_type
                (i_r_language, i_r_script, i_r_region, i_r_variants) = i_replacement

                if i_language is not None and i_language == r_language:
                    # This case currently only occurs when neither script nor region
                    # subtags are present. A single variant subtags may be present
                    # in |type|. And |i_type| definitely has a single variant subtag.
                    # Should this ever change, update this code accordingly.
                    assert type == (Any, None, None, None) or type == (
                        Any,
                        None,
                        None,
                        Any,
                    )
                    assert replacement == (Any, None, None, None)
                    assert i_type == (Any, None, None, Any)
                    assert i_replacement == (Any, None, None, None)

                    # This case happens for the rules
                    #   "zh-guoyu -> zh",
                    #   "zh-hakka -> hak", and
                    #   "und-hakka -> und".
                    # Given the possible input "zh-guoyu-hakka", the first rule will
                    # change it to "zh-hakka", and then the second rule can be
                    # applied. (The third rule isn't applied ever.)
                    #
                    # Let's assume there's a hypothetical rule
                    #   "zh-aaaaa" -> "en"
                    # And we have the input "zh-aaaaa-hakka", then "zh-aaaaa -> en"
                    # is applied before "zh-hakka -> hak", because rules are sorted
                    # alphabetically. That means the overall result is "en":
                    # "zh-aaaaa-hakka" is first canonicalized to "en-hakka" and then
                    # "hakka" is removed through the third rule.
                    #
                    # No current rule requires to handle this special case, so we
                    # don't yet support it.
                    assert variants is None or variants <= i_variants

                    # Combine all variants and remove duplicates.
                    vars = set(
                        i_variants.split("-")
                        + (variants.split("-") if variants else [])
                    )

                    # Add the variants alphabetically sorted.
                    n_type = (language, None, None, "-".join(sorted(vars)))

                    assert (
                        n_type not in transitive_rules
                        or transitive_rules[n_type] == i_replacement
                    )
                    transitive_rules[n_type] = i_replacement

                    continue

                if i_script is not None and i_script == r_script:
                    # This case currently doesn't occur, so we don't yet support it.
                    raise ValueError(
                        f"{type} -> {replacement} :: {i_type} -> {i_replacement}"
                    )
                if i_region is not None and i_region == r_region:
                    # This case currently only applies for sign language
                    # replacements. Similar to the language subtag case any other
                    # combination isn't currently supported.
                    assert type == (None, None, Any, None)
                    assert replacement == (None, None, Any, None)
                    assert i_type == ("sgn", None, Any, None)
                    assert i_replacement == (Any, None, None, None)

                    n_type = ("sgn", None, region, None)

                    assert n_type not in transitive_rules
                    transitive_rules[n_type] = i_replacement

                    continue

                if i_variants is not None and i_variants == r_variants:
                    # This case currently doesn't occur, so we don't yet support it.
                    raise ValueError(
                        f"{type} -> {replacement} :: {i_type} -> {i_replacement}"
                    )

        # Ensure there are no contradicting rules.
        assert all(
            rules[type] == replacement
            for (type, replacement) in transitive_rules.items()
            if type in rules
        )

        # If |transitive_rules| is not a subset of |rules|, new rules will be added.
        modified_rules = not (transitive_rules.keys() <= rules.keys())

        # Ensure we only have to iterate more than once for the "guoyo-{hakka,xiang}"
        # case. Failing this assertion means either there's a bug when computing the
        # stop condition of this loop or a new kind of legacy language tags was added.
        if modified_rules and loop_count > 1:
            new_rules = {k for k in transitive_rules.keys() if k not in rules}
            for k in new_rules:
                assert k == (Any, None, None, "guoyu-hakka") or k == (
                    Any,
                    None,
                    None,
                    "guoyu-xiang",
                )

        # Merge the transitive rules.
        rules.update(transitive_rules)

    # Computes the size of the union of all field value sets.
    def multi_map_size(locale_id):
        (language, script, region, variants) = locale_id

        return (
            (1 if language is not None else 0)
            + (1 if script is not None else 0)
            + (1 if region is not None else 0)
            + (len(variants.split("-")) if variants is not None else 0)
        )

    # Dictionary of legacy mappings, contains raw rules, e.g.
    # (None, None, None, "hepburn-heploc") -> (None, None, None, "alalc97").
    legacy_mappings = {}

    # Dictionary of simple language subtag mappings, e.g. "in" -> "id".
    language_mappings = {}

    # Dictionary of complex language subtag mappings, modifying more than one
    # subtag, e.g. "sh" -> ("sr", "Latn", None) and "cnr" -> ("sr", None, "ME").
    complex_language_mappings = {}

    # Dictionary of simple script subtag mappings, e.g. "Qaai" -> "Zinh".
    script_mappings = {}

    # Dictionary of simple region subtag mappings, e.g. "DD" -> "DE".
    region_mappings = {}

    # Dictionary of complex region subtag mappings, containing more than one
    # replacement, e.g. "SU" -> ("RU", ["AM", "AZ", "BY", ...]).
    complex_region_mappings = {}

    # Dictionary of aliased variant subtags to a tuple of preferred replacement
    # type and replacement, e.g. "arevela" -> ("language", "hy") or
    # "aaland" -> ("region", "AX") or "heploc" -> ("variant", "alalc97").
    variant_mappings = {}

    # Preprocess all rules so we can perform a single lookup per subtag at runtime.
    for type, replacement in rules.items():
        (language, script, region, variants) = type
        (r_language, r_script, r_region, r_variants) = replacement

        type_map_size = multi_map_size(type)

        # Most mappings are one-to-one and can be encoded through lookup tables.
        if type_map_size == 1:
            if language is not None:
                assert r_language is not None, "Can't remove a language subtag"

                # We don't yet support this case.
                assert (
                    r_variants is None
                ), f"Unhandled variant replacement in language alias: {replacement}"

                if replacement == (Any, None, None, None):
                    language_mappings[language] = r_language
                else:
                    complex_language_mappings[language] = replacement[:-1]
            elif script is not None:
                # We don't support removing script subtags.
                assert (
                    r_script is not None
                ), f"Can't remove a script subtag: {replacement}"

                # We only support one-to-one script mappings for now.
                assert replacement == (
                    None,
                    Any,
                    None,
                    None,
                ), f"Unhandled replacement in script alias: {replacement}"

                script_mappings[script] = r_script
            elif region is not None:
                # We don't support removing region subtags.
                assert (
                    r_region is not None
                ), f"Can't remove a region subtag: {replacement}"

                # We only support one-to-one region mappings for now.
                assert replacement == (
                    None,
                    None,
                    Any,
                    None,
                ), f"Unhandled replacement in region alias: {replacement}"

                if type not in territory_exception_rules:
                    region_mappings[region] = r_region
                else:
                    complex_region_mappings[region] = [
                        r_region
                        for (_, _, r_region, _) in territory_exception_rules[type]
                    ]
            else:
                assert variants is not None
                assert len(variants.split("-")) == 1

                # We only support one-to-one variant mappings for now.
                assert (
                    multi_map_size(replacement) <= 1
                ), f"Unhandled replacement in variant alias: {replacement}"

                if r_language is not None:
                    variant_mappings[variants] = ("language", r_language)
                elif r_script is not None:
                    variant_mappings[variants] = ("script", r_script)
                elif r_region is not None:
                    variant_mappings[variants] = ("region", r_region)
                elif r_variants is not None:
                    assert len(r_variants.split("-")) == 1
                    variant_mappings[variants] = ("variant", r_variants)
                else:
                    variant_mappings[variants] = None
        else:
            # Alias rules which have multiple input fields must be processed
            # first. This applies only to a handful of rules, so our generated
            # code adds fast paths to skip these rules in the common case.

            # Case 1: Language and at least one variant subtag.
            if language is not None and variants is not None:
                pass

            # Case 2: Sign language and a region subtag.
            elif language == "sgn" and region is not None:
                pass

            # Case 3: "hepburn-heploc" to "alalc97" canonicalization.
            elif (
                language is None
                and variants is not None
                and len(variants.split("-")) == 2
            ):
                pass

            # Any other combination is currently unsupported.
            else:
                raise ValueError(f"{type} -> {replacement}")

            legacy_mappings[type] = replacement

    tree = ET.parse(core_file.open("common/supplemental/likelySubtags.xml"))

    likely_subtags = {}

    for likely_subtag in tree.iterfind(".//likelySubtag"):
        from_tag = bcp47_id(likely_subtag.get("from"))
        from_match = re_unicode_language_id.match(from_tag)
        assert (
            from_match is not None
        ), f"{from_tag} invalid Unicode BCP 47 locale identifier"
        assert (
            from_match.group("variants") is None
        ), f"unexpected variant subtags in {from_tag}"

        to_tag = bcp47_id(likely_subtag.get("to"))
        to_match = re_unicode_language_id.match(to_tag)
        assert (
            to_match is not None
        ), f"{to_tag} invalid Unicode BCP 47 locale identifier"
        assert (
            to_match.group("variants") is None
        ), f"unexpected variant subtags in {to_tag}"

        from_canonical = bcp47_canonical(
            *from_match.group("language", "script", "region", "variants")
        )

        to_canonical = bcp47_canonical(
            *to_match.group("language", "script", "region", "variants")
        )

        # Remove the empty variant subtags.
        from_canonical = from_canonical[:-1]
        to_canonical = to_canonical[:-1]

        likely_subtags[from_canonical] = to_canonical

    complex_region_mappings_final = {}

    for deprecated_region, replacements in complex_region_mappings.items():
        # Find all likely subtag entries which don't already contain a region
        # subtag and whose target region is in the list of replacement regions.
        region_likely_subtags = [
            (from_language, from_script, to_region)
            for (
                (from_language, from_script, from_region),
                (_, _, to_region),
            ) in likely_subtags.items()
            if from_region is None and to_region in replacements
        ]

        # The first replacement entry is the default region.
        default = replacements[0]

        # Find all likely subtag entries whose region matches the default region.
        default_replacements = {
            (language, script)
            for (language, script, region) in region_likely_subtags
            if region == default
        }

        # And finally find those entries which don't use the default region.
        # These are the entries we're actually interested in, because those need
        # to be handled specially when selecting the correct preferred region.
        non_default_replacements = [
            (language, script, region)
            for (language, script, region) in region_likely_subtags
            if (language, script) not in default_replacements
        ]

        # Remove redundant mappings.
        #
        # For example starting with CLDR 43, the deprecated region "SU" has the
        # following non-default replacement entries for "GE":
        # - ('sva', None, 'GE')
        # - ('sva', 'Cyrl', 'GE')
        # - ('sva', 'Latn', 'GE')
        #
        # The latter two entries are redundant, because they're already handled
        # by the first entry.
        non_default_replacements = [
            (language, script, region)
            for (language, script, region) in non_default_replacements
            if script is None
            or (language, None, region) not in non_default_replacements
        ]

        # If there are no non-default replacements, we can handle the region as
        # part of the simple region mapping.
        if non_default_replacements:
            complex_region_mappings_final[deprecated_region] = (
                default,
                non_default_replacements,
            )
        else:
            region_mappings[deprecated_region] = default

    return {
        "legacyMappings": legacy_mappings,
        "languageMappings": language_mappings,
        "complexLanguageMappings": complex_language_mappings,
        "scriptMappings": script_mappings,
        "regionMappings": region_mappings,
        "complexRegionMappings": complex_region_mappings_final,
        "variantMappings": variant_mappings,
        "likelySubtags": likely_subtags,
    }


def readUnicodeExtensions(core_file):
    import xml.etree.ElementTree as ET

    # Match all xml-files in the BCP 47 directory.
    bcpFileRE = re.compile(r"^common/bcp47/.+\.xml$")

    # https://www.unicode.org/reports/tr35/#Unicode_locale_identifier
    #
    # type = alphanum{3,8} (sep alphanum{3,8})* ;
    typeRE = re.compile(r"^[a-z0-9]{3,8}(-[a-z0-9]{3,8})*$")

    # https://www.unicode.org/reports/tr35/#Unicode_language_identifier
    #
    # unicode_region_subtag = alpha{2} ;
    alphaRegionRE = re.compile(r"^[A-Z]{2}$", re.IGNORECASE)

    # Mapping from Unicode extension types to dict of deprecated to
    # preferred values.
    mapping = {
        # Unicode BCP 47 U Extension
        "u": {},
        # Unicode BCP 47 T Extension
        "t": {},
    }

    def readBCP47File(file):
        tree = ET.parse(file)
        for keyword in tree.iterfind(".//keyword/key"):
            extension = keyword.get("extension", "u")
            assert (
                extension == "u" or extension == "t"
            ), "unknown extension type: {}".format(extension)

            extension_name = keyword.get("name")

            for type in keyword.iterfind("type"):
                # <https://unicode.org/reports/tr35/#Unicode_Locale_Extension_Data_Files>:
                #
                # The key or type name used by Unicode locale extension with 'u' extension
                # syntax or the 't' extensions syntax. When alias below is absent, this name
                # can be also used with the old style "@key=type" syntax.
                name = type.get("name")

                # Ignore the special name:
                # - <https://unicode.org/reports/tr35/#CODEPOINTS>
                # - <https://unicode.org/reports/tr35/#REORDER_CODE>
                # - <https://unicode.org/reports/tr35/#RG_KEY_VALUE>
                # - <https://unicode.org/reports/tr35/#SCRIPT_CODE>
                # - <https://unicode.org/reports/tr35/#SUBDIVISION_CODE>
                # - <https://unicode.org/reports/tr35/#PRIVATE_USE>
                if name in (
                    "CODEPOINTS",
                    "REORDER_CODE",
                    "RG_KEY_VALUE",
                    "SCRIPT_CODE",
                    "SUBDIVISION_CODE",
                    "PRIVATE_USE",
                ):
                    continue

                # All other names should match the 'type' production.
                assert (
                    typeRE.match(name) is not None
                ), "{} matches the 'type' production".format(name)

                # <https://unicode.org/reports/tr35/#Unicode_Locale_Extension_Data_Files>:
                #
                # The preferred value of the deprecated key, type or attribute element.
                # When a key, type or attribute element is deprecated, this attribute is
                # used for specifying a new canonical form if available.
                preferred = type.get("preferred")

                # <https://unicode.org/reports/tr35/#Unicode_Locale_Extension_Data_Files>:
                #
                # The BCP 47 form is the canonical form, and recommended. Other aliases are
                # included only for backwards compatibility.
                alias = type.get("alias")

                # <https://unicode.org/reports/tr35/#Canonical_Unicode_Locale_Identifiers>
                #
                # Use the bcp47 data to replace keys, types, tfields, and tvalues by their
                # canonical forms. See Section 3.6.4 U Extension Data Files) and Section
                # 3.7.1 T Extension Data Files. The aliases are in the alias attribute
                # value, while the canonical is in the name attribute value.

                # 'preferred' contains the new preferred name, 'alias' the compatibility
                # name, but then there's this entry where 'preferred' and 'alias' are the
                # same. So which one to choose? Assume 'preferred' is the actual canonical
                # name.
                #
                # <type name="islamicc"
                #       description="Civil (algorithmic) Arabic calendar"
                #       deprecated="true"
                #       preferred="islamic-civil"
                #       alias="islamic-civil"/>

                if preferred is not None:
                    assert typeRE.match(preferred), preferred
                    mapping[extension].setdefault(extension_name, {})[name] = preferred

                if alias is not None:
                    for alias_name in alias.lower().split(" "):
                        # Ignore alias entries which don't match the 'type' production.
                        if typeRE.match(alias_name) is None:
                            continue

                        # See comment above when 'alias' and 'preferred' are both present.
                        if (
                            preferred is not None
                            and name in mapping[extension][extension_name]
                        ):
                            continue

                        # Skip over entries where 'name' and 'alias' are equal.
                        #
                        # <type name="pst8pdt"
                        #       description="POSIX style time zone for US Pacific Time"
                        #       alias="PST8PDT"
                        #       since="1.8"/>
                        if name == alias_name:
                            continue

                        mapping[extension].setdefault(extension_name, {})[
                            alias_name
                        ] = name

    def readSupplementalMetadata(file):
        # Find subdivision and region replacements.
        #
        # <https://www.unicode.org/reports/tr35/#Canonical_Unicode_Locale_Identifiers>
        #
        # Replace aliases in special key values:
        #   - If there is an 'sd' or 'rg' key, replace any subdivision alias
        #     in its value in the same way, using subdivisionAlias data.
        tree = ET.parse(file)
        for alias in tree.iterfind(".//subdivisionAlias"):
            type = alias.get("type")
            assert (
                typeRE.match(type) is not None
            ), "{} matches the 'type' production".format(type)

            # Take the first replacement when multiple ones are present.
            replacement = alias.get("replacement").split(" ")[0].lower()

            # Append "zzzz" if the replacement is a two-letter region code.
            if alphaRegionRE.match(replacement) is not None:
                replacement += "zzzz"

            # Assert the replacement is syntactically correct.
            assert (
                typeRE.match(replacement) is not None
            ), "replacement {} matches the 'type' production".format(replacement)

            # 'subdivisionAlias' applies to 'rg' and 'sd' keys.
            mapping["u"].setdefault("rg", {})[type] = replacement
            mapping["u"].setdefault("sd", {})[type] = replacement

    for name in core_file.namelist():
        if bcpFileRE.match(name):
            readBCP47File(core_file.open(name))

    readSupplementalMetadata(
        core_file.open("common/supplemental/supplementalMetadata.xml")
    )

    return {
        "unicodeMappings": mapping["u"],
        "transformMappings": mapping["t"],
    }


def writeCLDRLanguageTagData(println, data, url):
    """Writes the language tag data to the Intl data file."""

    println(generatedFileWarning)
    println("// Version: CLDR-{}".format(data["version"]))
    println("// URL: {}".format(url))

    println(
        """
#include "mozilla/Assertions.h"
#include "mozilla/Span.h"
#include "mozilla/TextUtils.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <type_traits>

#include "mozilla/intl/Locale.h"

using namespace mozilla::intl::LanguageTagLimits;

template <size_t Length, size_t TagLength, size_t SubtagLength>
static inline bool HasReplacement(
    const char (&subtags)[Length][TagLength],
    const mozilla::intl::LanguageTagSubtag<SubtagLength>& subtag) {
  MOZ_ASSERT(subtag.Length() == TagLength - 1,
             "subtag must have the same length as the list of subtags");

  const char* ptr = subtag.Span().data();
  return std::binary_search(std::begin(subtags), std::end(subtags), ptr,
                            [](const char* a, const char* b) {
                              return memcmp(a, b, TagLength - 1) < 0;
                            });
}

template <size_t Length, size_t TagLength, size_t SubtagLength>
static inline const char* SearchReplacement(
    const char (&subtags)[Length][TagLength], const char* (&aliases)[Length],
    const mozilla::intl::LanguageTagSubtag<SubtagLength>& subtag) {
  MOZ_ASSERT(subtag.Length() == TagLength - 1,
             "subtag must have the same length as the list of subtags");

  const char* ptr = subtag.Span().data();
  auto p = std::lower_bound(std::begin(subtags), std::end(subtags), ptr,
                            [](const char* a, const char* b) {
                              return memcmp(a, b, TagLength - 1) < 0;
                            });
  if (p != std::end(subtags) && memcmp(*p, ptr, TagLength - 1) == 0) {
    return aliases[std::distance(std::begin(subtags), p)];
  }
  return nullptr;
}

#ifdef DEBUG
static bool IsAsciiLowercaseAlphanumeric(char c) {
  return mozilla::IsAsciiLowercaseAlpha(c) || mozilla::IsAsciiDigit(c);
}

static bool IsAsciiLowercaseAlphanumericOrDash(char c) {
  return IsAsciiLowercaseAlphanumeric(c) || c == '-';
}

static bool IsCanonicallyCasedLanguageTag(mozilla::Span<const char> span) {
  return std::all_of(span.begin(), span.end(),
                     mozilla::IsAsciiLowercaseAlpha<char>);
}

static bool IsCanonicallyCasedScriptTag(mozilla::Span<const char> span) {
  return mozilla::IsAsciiUppercaseAlpha(span[0]) &&
         std::all_of(span.begin() + 1, span.end(),
                     mozilla::IsAsciiLowercaseAlpha<char>);
}

static bool IsCanonicallyCasedRegionTag(mozilla::Span<const char> span) {
  return std::all_of(span.begin(), span.end(),
                     mozilla::IsAsciiUppercaseAlpha<char>) ||
         std::all_of(span.begin(), span.end(), mozilla::IsAsciiDigit<char>);
}

static bool IsCanonicallyCasedVariantTag(mozilla::Span<const char> span) {
  return std::all_of(span.begin(), span.end(), IsAsciiLowercaseAlphanumeric);
}

static bool IsCanonicallyCasedUnicodeKey(mozilla::Span<const char> key) {
  return std::all_of(key.begin(), key.end(), IsAsciiLowercaseAlphanumeric);
}

static bool IsCanonicallyCasedUnicodeType(mozilla::Span<const char> type) {
  return std::all_of(type.begin(), type.end(),
                     IsAsciiLowercaseAlphanumericOrDash);
}

static bool IsCanonicallyCasedTransformKey(mozilla::Span<const char> key) {
  return std::all_of(key.begin(), key.end(), IsAsciiLowercaseAlphanumeric);
}

static bool IsCanonicallyCasedTransformType(mozilla::Span<const char> type) {
  return std::all_of(type.begin(), type.end(),
                     IsAsciiLowercaseAlphanumericOrDash);
}
#endif
""".rstrip()
    )

    source = "CLDR Supplemental Data, version {}".format(data["version"])
    legacy_mappings = data["legacyMappings"]
    language_mappings = data["languageMappings"]
    complex_language_mappings = data["complexLanguageMappings"]
    script_mappings = data["scriptMappings"]
    region_mappings = data["regionMappings"]
    complex_region_mappings = data["complexRegionMappings"]
    variant_mappings = data["variantMappings"]
    unicode_mappings = data["unicodeMappings"]
    transform_mappings = data["transformMappings"]

    # unicode_language_subtag = alpha{2,3} | alpha{5,8} ;
    language_maxlength = 8

    # unicode_script_subtag = alpha{4} ;
    script_maxlength = 4

    # unicode_region_subtag = (alpha{2} | digit{3}) ;
    region_maxlength = 3

    writeMappingsBinarySearch(
        println,
        "LanguageMapping",
        "LanguageSubtag&",
        "language",
        "IsStructurallyValidLanguageTag",
        "IsCanonicallyCasedLanguageTag",
        language_mappings,
        language_maxlength,
        "Mappings from language subtags to preferred values.",
        source,
        url,
    )
    writeMappingsBinarySearch(
        println,
        "ComplexLanguageMapping",
        "const LanguageSubtag&",
        "language",
        "IsStructurallyValidLanguageTag",
        "IsCanonicallyCasedLanguageTag",
        complex_language_mappings.keys(),
        language_maxlength,
        "Language subtags with complex mappings.",
        source,
        url,
    )
    writeMappingsBinarySearch(
        println,
        "ScriptMapping",
        "ScriptSubtag&",
        "script",
        "IsStructurallyValidScriptTag",
        "IsCanonicallyCasedScriptTag",
        script_mappings,
        script_maxlength,
        "Mappings from script subtags to preferred values.",
        source,
        url,
    )
    writeMappingsBinarySearch(
        println,
        "RegionMapping",
        "RegionSubtag&",
        "region",
        "IsStructurallyValidRegionTag",
        "IsCanonicallyCasedRegionTag",
        region_mappings,
        region_maxlength,
        "Mappings from region subtags to preferred values.",
        source,
        url,
    )
    writeMappingsBinarySearch(
        println,
        "ComplexRegionMapping",
        "const RegionSubtag&",
        "region",
        "IsStructurallyValidRegionTag",
        "IsCanonicallyCasedRegionTag",
        complex_region_mappings.keys(),
        region_maxlength,
        "Region subtags with complex mappings.",
        source,
        url,
    )

    writeComplexLanguageTagMappings(
        println,
        complex_language_mappings,
        "Language subtags with complex mappings.",
        source,
        url,
    )
    writeComplexRegionTagMappings(
        println,
        complex_region_mappings,
        "Region subtags with complex mappings.",
        source,
        url,
    )

    writeVariantTagMappings(
        println,
        variant_mappings,
        "Mappings from variant subtags to preferred values.",
        source,
        url,
    )

    writeLegacyMappingsFunction(
        println, legacy_mappings, "Canonicalize legacy locale identifiers.", source, url
    )

    writeSignLanguageMappingsFunction(
        println, legacy_mappings, "Mappings from legacy sign languages.", source, url
    )

    writeUnicodeExtensionsMappings(println, unicode_mappings, "Unicode")
    writeUnicodeExtensionsMappings(println, transform_mappings, "Transform")


def writeCLDRLanguageTagLikelySubtagsTest(println, data, url):
    """Writes the likely-subtags test file."""

    println(generatedFileWarning)

    source = "CLDR Supplemental Data, version {}".format(data["version"])
    language_mappings = data["languageMappings"]
    complex_language_mappings = data["complexLanguageMappings"]
    script_mappings = data["scriptMappings"]
    region_mappings = data["regionMappings"]
    complex_region_mappings = data["complexRegionMappings"]
    likely_subtags = data["likelySubtags"]

    def bcp47(tag):
        (language, script, region) = tag
        return "{}{}{}".format(
            language, "-" + script if script else "", "-" + region if region else ""
        )

    def canonical(tag):
        (language, script, region) = tag

        # Map deprecated language subtags.
        if language in language_mappings:
            language = language_mappings[language]
        elif language in complex_language_mappings:
            (language2, script2, region2) = complex_language_mappings[language]
            (language, script, region) = (
                language2,
                script if script else script2,
                region if region else region2,
            )

        # Map deprecated script subtags.
        if script in script_mappings:
            script = script_mappings[script]

        # Map deprecated region subtags.
        if region in region_mappings:
            region = region_mappings[region]
        else:
            # Assume no complex region mappings are needed for now.
            assert (
                region not in complex_region_mappings
            ), "unexpected region with complex mappings: {}".format(region)

        return (language, script, region)

    # https://unicode.org/reports/tr35/#Likely_Subtags

    def addLikelySubtags(tag):
        # Step 1: Canonicalize.
        (language, script, region) = canonical(tag)
        if script == "Zzzz":
            script = None
        if region == "ZZ":
            region = None

        # Step 2: Lookup.
        searches = (
            (language, script, region),
            (language, None, region),
            (language, script, None),
            (language, None, None),
            ("und", script, None),
        )
        search = next(search for search in searches if search in likely_subtags)

        (language_s, script_s, region_s) = search
        (language_m, script_m, region_m) = likely_subtags[search]

        # Step 3: Return.
        return (
            language if language != language_s else language_m,
            script if script != script_s else script_m,
            region if region != region_s else region_m,
        )

    # https://unicode.org/reports/tr35/#Likely_Subtags
    def removeLikelySubtags(tag):
        # Step 1: Add likely subtags.
        max = addLikelySubtags(tag)

        # Step 2: Remove variants (doesn't apply here).

        # Step 3: Find a match.
        (language, script, region) = max
        for trial in (
            (language, None, None),
            (language, None, region),
            (language, script, None),
        ):
            if addLikelySubtags(trial) == max:
                return trial

        # Step 4: Return maximized if no match found.
        return max

    def likely_canonical(from_tag, to_tag):
        # Canonicalize the input tag.
        from_tag = canonical(from_tag)

        # Update the expected result if necessary.
        if from_tag in likely_subtags:
            to_tag = likely_subtags[from_tag]

        # Canonicalize the expected output.
        to_canonical = canonical(to_tag)

        # Sanity check: This should match the result of |addLikelySubtags|.
        assert to_canonical == addLikelySubtags(from_tag)

        return to_canonical

    # |likely_subtags| contains non-canonicalized tags, so canonicalize it first.
    likely_subtags_canonical = {
        k: likely_canonical(k, v) for (k, v) in likely_subtags.items()
    }

    # Add test data for |Intl.Locale.prototype.maximize()|.
    writeMappingsVar(
        println,
        {bcp47(k): bcp47(v) for (k, v) in likely_subtags_canonical.items()},
        "maxLikelySubtags",
        "Extracted from likelySubtags.xml.",
        source,
        url,
    )

    # Use the maximalized tags as the input for the remove likely-subtags test.
    minimized = {
        tag: removeLikelySubtags(tag) for tag in likely_subtags_canonical.values()
    }

    # Add test data for |Intl.Locale.prototype.minimize()|.
    writeMappingsVar(
        println,
        {bcp47(k): bcp47(v) for (k, v) in minimized.items()},
        "minLikelySubtags",
        "Extracted from likelySubtags.xml.",
        source,
        url,
    )

    println(
        """
for (let [tag, maximal] of Object.entries(maxLikelySubtags)) {
    assertEq(new Intl.Locale(tag).maximize().toString(), maximal);
}"""
    )

    println(
        """
for (let [tag, minimal] of Object.entries(minLikelySubtags)) {
    assertEq(new Intl.Locale(tag).minimize().toString(), minimal);
}"""
    )

    println(
        """
if (typeof reportCompare === "function")
    reportCompare(0, 0);"""
    )


def readCLDRVersionFromICU():
    icuDir = os.path.join(topsrcdir, "intl/icu/source")
    if not os.path.isdir(icuDir):
        raise RuntimeError("not a directory: {}".format(icuDir))

    reVersion = re.compile(r'\s*cldrVersion\{"(\d+(?:\.\d+)?)"\}')

    for line in flines(os.path.join(icuDir, "data/misc/supplementalData.txt")):
        m = reVersion.match(line)
        if m:
            version = m.group(1)
            break

    if version is None:
        raise RuntimeError("can't resolve CLDR version")

    return version


def updateCLDRLangTags(args):
    """Update the LanguageTagGenerated.cpp file."""
    version = args.version
    url = args.url
    out = args.out
    filename = args.file

    # Determine current CLDR version from ICU.
    if version is None:
        version = readCLDRVersionFromICU()

    url = url.replace("<VERSION>", version)

    print("Arguments:")
    print("\tCLDR version: %s" % version)
    print("\tDownload url: %s" % url)
    if filename is not None:
        print("\tLocal CLDR common.zip file: %s" % filename)
    print("\tOutput file: %s" % out)
    print("")

    data = {
        "version": version,
    }

    def readFiles(cldr_file):
        with ZipFile(cldr_file) as zip_file:
            data.update(readSupplementalData(zip_file))
            data.update(readUnicodeExtensions(zip_file))

    print("Processing CLDR data...")
    if filename is not None:
        print("Always make sure you have the newest CLDR common.zip!")
        with open(filename, "rb") as cldr_file:
            readFiles(cldr_file)
    else:
        print("Downloading CLDR common.zip...")
        with closing(urlopen(url)) as cldr_file:
            cldr_data = io.BytesIO(cldr_file.read())
            readFiles(cldr_data)

    print("Writing Intl data...")
    with io.open(out, mode="w", encoding="utf-8", newline="") as f:
        println = partial(print, file=f)

        writeCLDRLanguageTagData(println, data, url)

    print("Writing Intl test data...")
    js_src_builtin_intl_dir = os.path.dirname(os.path.abspath(__file__))
    test_file = os.path.join(
        js_src_builtin_intl_dir,
        "../../tests/non262/Intl/Locale/likely-subtags-generated.js",
    )
    with io.open(test_file, mode="w", encoding="utf-8", newline="") as f:
        println = partial(print, file=f)

        println("// |reftest| skip-if(!this.hasOwnProperty('Intl'))")
        writeCLDRLanguageTagLikelySubtagsTest(println, data, url)


def flines(filepath, encoding="utf-8"):
    """Open filepath and iterate over its content."""
    with io.open(filepath, mode="r", encoding=encoding) as f:
        for line in f:
            yield line


@total_ordering
class Zone(object):
    """Time zone with optional file name."""

    def __init__(self, name, filename=""):
        self.name = name
        self.filename = filename

    def __eq__(self, other):
        return hasattr(other, "name") and self.name == other.name

    def __lt__(self, other):
        return self.name < other.name

    def __hash__(self):
        return hash(self.name)

    def __str__(self):
        return self.name

    def __repr__(self):
        return self.name


class TzDataDir(object):
    """tzdata source from a directory."""

    def __init__(self, obj):
        self.name = partial(os.path.basename, obj)
        self.resolve = partial(os.path.join, obj)
        self.basename = os.path.basename
        self.isfile = os.path.isfile
        self.listdir = partial(os.listdir, obj)
        self.readlines = flines


class TzDataFile(object):
    """tzdata source from a file (tar or gzipped)."""

    def __init__(self, obj):
        self.name = lambda: os.path.splitext(
            os.path.splitext(os.path.basename(obj))[0]
        )[0]
        self.resolve = obj.getmember
        self.basename = attrgetter("name")
        self.isfile = tarfile.TarInfo.isfile
        self.listdir = obj.getnames
        self.readlines = partial(self._tarlines, obj)

    def _tarlines(self, tar, m):
        with closing(tar.extractfile(m)) as f:
            for line in f:
                yield line.decode("utf-8")


def validateTimeZones(zones, links):
    """Validate the zone and link entries."""
    linkZones = set(links.keys())
    intersect = linkZones.intersection(zones)
    if intersect:
        raise RuntimeError("Links also present in zones: %s" % intersect)

    zoneNames = {z.name for z in zones}
    linkTargets = set(links.values())
    if not linkTargets.issubset(zoneNames):
        raise RuntimeError(
            "Link targets not found: %s" % linkTargets.difference(zoneNames)
        )


def partition(iterable, *predicates):
    def innerPartition(pred, it):
        it1, it2 = tee(it)
        return (filter(pred, it1), filterfalse(pred, it2))

    if len(predicates) == 0:
        return iterable
    (left, right) = innerPartition(predicates[0], iterable)
    if len(predicates) == 1:
        return (left, right)
    return tuple([left] + list(partition(right, *predicates[1:])))


def listIANAFiles(tzdataDir):
    def isTzFile(d, m, f):
        return m(f) and d.isfile(d.resolve(f))

    return filter(
        partial(isTzFile, tzdataDir, re.compile("^[a-z0-9]+$").match),
        tzdataDir.listdir(),
    )


def readIANAFiles(tzdataDir, files):
    """Read all IANA time zone files from the given iterable."""
    nameSyntax = r"[\w/+\-]+"
    pZone = re.compile(r"Zone\s+(?P<name>%s)\s+.*" % nameSyntax)
    pLink = re.compile(
        r"Link\s+(?P<target>%s)\s+(?P<name>%s)(?:\s+#.*)?" % (nameSyntax, nameSyntax)
    )

    def createZone(line, fname):
        match = pZone.match(line)
        name = match.group("name")
        return Zone(name, fname)

    def createLink(line, fname):
        match = pLink.match(line)
        (name, target) = match.group("name", "target")
        return (Zone(name, fname), target)

    zones = set()
    links = dict()
    for filename in files:
        filepath = tzdataDir.resolve(filename)
        for line in tzdataDir.readlines(filepath):
            if line.startswith("Zone"):
                zones.add(createZone(line, filename))
            if line.startswith("Link"):
                (link, target) = createLink(line, filename)
                links[link] = target

    return (zones, links)


def readIANATimeZones(tzdataDir, ignoreBackzone, ignoreFactory):
    """Read the IANA time zone information from `tzdataDir`."""

    backzoneFiles = {"backzone"}
    (bkfiles, tzfiles) = partition(listIANAFiles(tzdataDir), backzoneFiles.__contains__)

    # Read zone and link infos.
    (zones, links) = readIANAFiles(tzdataDir, tzfiles)
    (backzones, backlinks) = readIANAFiles(tzdataDir, bkfiles)

    # Remove the placeholder time zone "Factory".
    if ignoreFactory:
        zones.remove(Zone("Factory"))

    # Merge with backzone data.
    if not ignoreBackzone:
        zones |= backzones
        links = {
            name: target for name, target in links.items() if name not in backzones
        }
        links.update(backlinks)

    validateTimeZones(zones, links)

    return (zones, links)


def readICUResourceFile(filename):
    """Read an ICU resource file.

    Yields (<table-name>, <startOrEnd>, <value>) for each table.
    """

    numberValue = r"-?\d+"
    stringValue = r'".+?"'

    def asVector(val):
        return r"%s(?:\s*,\s*%s)*" % (val, val)

    numberVector = asVector(numberValue)
    stringVector = asVector(stringValue)

    reNumberVector = re.compile(numberVector)
    reStringVector = re.compile(stringVector)
    reNumberValue = re.compile(numberValue)
    reStringValue = re.compile(stringValue)

    def parseValue(value):
        m = reNumberVector.match(value)
        if m:
            return [int(v) for v in reNumberValue.findall(value)]
        m = reStringVector.match(value)
        if m:
            return [v[1:-1] for v in reStringValue.findall(value)]
        raise RuntimeError("unknown value type: %s" % value)

    def extractValue(values):
        if len(values) == 0:
            return None
        if len(values) == 1:
            return values[0]
        return values

    def line(*args):
        maybeMultiComments = r"(?:/\*[^*]*\*/)*"
        maybeSingleComment = r"(?://.*)?"
        lineStart = "^%s" % maybeMultiComments
        lineEnd = r"%s\s*%s$" % (maybeMultiComments, maybeSingleComment)
        return re.compile(r"\s*".join(chain([lineStart], args, [lineEnd])))

    tableName = r'(?P<quote>"?)(?P<name>.+?)(?P=quote)'
    tableValue = r"(?P<value>%s|%s)" % (numberVector, stringVector)

    reStartTable = line(tableName, r"\{")
    reEndTable = line(r"\}")
    reSingleValue = line(r",?", tableValue, r",?")
    reCompactTable = line(tableName, r"\{", tableValue, r"\}")
    reEmptyLine = line()

    tables = []

    def currentTable():
        return "|".join(tables)

    values = []
    for line in flines(filename, "utf-8-sig"):
        line = line.strip()
        if line == "":
            continue

        m = reEmptyLine.match(line)
        if m:
            continue

        m = reStartTable.match(line)
        if m:
            assert len(values) == 0
            tables.append(m.group("name"))
            continue

        m = reEndTable.match(line)
        if m:
            yield (currentTable(), extractValue(values))
            tables.pop()
            values = []
            continue

        m = reCompactTable.match(line)
        if m:
            assert len(values) == 0
            tables.append(m.group("name"))
            yield (currentTable(), extractValue(parseValue(m.group("value"))))
            tables.pop()
            continue

        m = reSingleValue.match(line)
        if m and tables:
            values.extend(parseValue(m.group("value")))
            continue

        raise RuntimeError("unknown entry: %s" % line)


def readICUTimeZonesFromTimezoneTypes(icuTzDir):
    """Read the ICU time zone information from `icuTzDir`/timezoneTypes.txt
    and returns the tuple (zones, links).
    """
    typeMapTimeZoneKey = "timezoneTypes:table(nofallback)|typeMap|timezone|"
    typeAliasTimeZoneKey = "timezoneTypes:table(nofallback)|typeAlias|timezone|"

    def toTimeZone(name):
        return Zone(name.replace(":", "/"))

    zones = set()
    links = dict()

    for name, value in readICUResourceFile(os.path.join(icuTzDir, "timezoneTypes.txt")):
        if name.startswith(typeMapTimeZoneKey):
            zones.add(toTimeZone(name[len(typeMapTimeZoneKey) :]))
        if name.startswith(typeAliasTimeZoneKey):
            links[toTimeZone(name[len(typeAliasTimeZoneKey) :])] = value

    validateTimeZones(zones, links)

    return (zones, links)


def readICUTimeZonesFromZoneInfo(icuTzDir):
    """Read the ICU time zone information from `icuTzDir`/zoneinfo64.txt
    and returns the tuple (zones, links).
    """
    zoneKey = "zoneinfo64:table(nofallback)|Zones:array|:table"
    linkKey = "zoneinfo64:table(nofallback)|Zones:array|:int"
    namesKey = "zoneinfo64:table(nofallback)|Names"

    tzId = 0
    tzLinks = dict()
    tzNames = []

    for name, value in readICUResourceFile(os.path.join(icuTzDir, "zoneinfo64.txt")):
        if name == zoneKey:
            tzId += 1
        elif name == linkKey:
            tzLinks[tzId] = int(value)
            tzId += 1
        elif name == namesKey:
            tzNames.extend(value)

    links = {Zone(tzNames[zone]): tzNames[target] for (zone, target) in tzLinks.items()}
    zones = {Zone(v) for v in tzNames if Zone(v) not in links}

    validateTimeZones(zones, links)

    return (zones, links)


def readICUTimeZones(icuDir, icuTzDir, ignoreFactory):
    # zoneinfo64.txt contains the supported time zones by ICU. This data is
    # generated from tzdata files, it doesn't include "backzone" in stock ICU.
    (zoneinfoZones, zoneinfoLinks) = readICUTimeZonesFromZoneInfo(icuTzDir)

    # timezoneTypes.txt contains the canonicalization information for ICU. This
    # data is generated from CLDR files. It includes data about time zones from
    # tzdata's "backzone" file.
    (typesZones, typesLinks) = readICUTimeZonesFromTimezoneTypes(icuTzDir)

    # Remove the placeholder time zone "Factory".
    # See also <https://github.com/eggert/tz/blob/master/factory>.
    if ignoreFactory:
        zoneinfoZones.remove(Zone("Factory"))

    # Remove the ICU placeholder time zone "Etc/Unknown".
    # See also <https://unicode.org/reports/tr35/#Time_Zone_Identifiers>.
    for zones in (zoneinfoZones, typesZones):
        zones.remove(Zone("Etc/Unknown"))

    # Remove any outdated ICU links.
    for links in (zoneinfoLinks, typesLinks):
        for zone in otherICULegacyLinks().keys():
            if zone not in links:
                raise KeyError(f"Can't remove non-existent link from '{zone}'")
            del links[zone]

    # Information in zoneinfo64 should be a superset of timezoneTypes.
    def inZoneInfo64(zone):
        return zone in zoneinfoZones or zone in zoneinfoLinks

    notFoundInZoneInfo64 = [zone for zone in typesZones if not inZoneInfo64(zone)]
    if notFoundInZoneInfo64:
        raise RuntimeError(
            "Missing time zones in zoneinfo64.txt: %s" % notFoundInZoneInfo64
        )

    notFoundInZoneInfo64 = [
        zone for zone in typesLinks.keys() if not inZoneInfo64(zone)
    ]
    if notFoundInZoneInfo64:
        raise RuntimeError(
            "Missing time zones in zoneinfo64.txt: %s" % notFoundInZoneInfo64
        )

    # zoneinfo64.txt only defines the supported time zones by ICU, the canonicalization
    # rules are defined through timezoneTypes.txt. Merge both to get the actual zones
    # and links used by ICU.
    icuZones = set(
        chain(
            (zone for zone in zoneinfoZones if zone not in typesLinks),
            (zone for zone in typesZones),
        )
    )
    icuLinks = dict(
        chain(
            (
                (zone, target)
                for (zone, target) in zoneinfoLinks.items()
                if zone not in typesZones
            ),
            ((zone, target) for (zone, target) in typesLinks.items()),
        )
    )

    return (icuZones, icuLinks)


def readICULegacyZones(icuDir):
    """Read the ICU legacy time zones from `icuTzDir`/tools/tzcode/icuzones
    and returns the tuple (zones, links).
    """
    tzdir = TzDataDir(os.path.join(icuDir, "tools/tzcode"))

    # Per spec we must recognize only IANA time zones and links, but ICU
    # recognizes various legacy, non-IANA time zones and links. Compute these
    # non-IANA time zones and links.

    # Most legacy, non-IANA time zones and links are in the icuzones file.
    (zones, links) = readIANAFiles(tzdir, ["icuzones"])

    # Remove the ICU placeholder time zone "Etc/Unknown".
    # See also <https://unicode.org/reports/tr35/#Time_Zone_Identifiers>.
    zones.remove(Zone("Etc/Unknown"))

    # A handful of non-IANA zones/links are not in icuzones and must be added
    # manually so that we won't invoke ICU with them.
    for zone, target in otherICULegacyLinks().items():
        if zone in links:
            if links[zone] != target:
                raise KeyError(
                    f"Can't overwrite link '{zone} -> {links[zone]}' with '{target}'"
                )
            else:
                print(
                    f"Info: Link '{zone} -> {target}' can be removed from otherICULegacyLinks()"
                )
        links[zone] = target

    return (zones, links)


def otherICULegacyLinks():
    """The file `icuTzDir`/tools/tzcode/icuzones contains all ICU legacy time
    zones with the exception of time zones which are removed by IANA after an
    ICU release.

    For example ICU 67 uses tzdata2018i, but tzdata2020b removed the link from
    "US/Pacific-New" to "America/Los_Angeles". ICU standalone tzdata updates
    don't include modified icuzones files, so we must manually record any IANA
    modifications here.

    After an ICU update, we can remove any no longer needed entries from this
    function by checking if the relevant entries are now included in icuzones.
    """

    return {
        # Current ICU is up-to-date with IANA, so this dict is empty.
    }


def icuTzDataVersion(icuTzDir):
    """Read the ICU time zone version from `icuTzDir`/zoneinfo64.txt."""

    def searchInFile(pattern, f):
        p = re.compile(pattern)
        for line in flines(f, "utf-8-sig"):
            m = p.search(line)
            if m:
                return m.group(1)
        return None

    zoneinfo = os.path.join(icuTzDir, "zoneinfo64.txt")
    if not os.path.isfile(zoneinfo):
        raise RuntimeError("file not found: %s" % zoneinfo)
    version = searchInFile(r"^//\s+tz version:\s+([0-9]{4}[a-z])$", zoneinfo)
    if version is None:
        raise RuntimeError(
            "%s does not contain a valid tzdata version string" % zoneinfo
        )
    return version


def findIncorrectICUZones(ianaZones, ianaLinks, icuZones, icuLinks, ignoreBackzone):
    """Find incorrect ICU zone entries."""

    def isIANATimeZone(zone):
        return zone in ianaZones or zone in ianaLinks

    def isICUTimeZone(zone):
        return zone in icuZones or zone in icuLinks

    def isICULink(zone):
        return zone in icuLinks

    # All IANA zones should be present in ICU.
    missingTimeZones = [zone for zone in ianaZones if not isICUTimeZone(zone)]
    # Normally zones in backzone are also present as links in one of the other
    # time zone files. The only exception to this rule is the Asia/Hanoi time
    # zone, this zone is only present in the backzone file.
    expectedMissing = [] if ignoreBackzone else [Zone("Asia/Hanoi")]
    if missingTimeZones != expectedMissing:
        raise RuntimeError(
            "Not all zones are present in ICU, did you forget "
            "to run intl/update-tzdata.sh? %s" % missingTimeZones
        )

    # Zones which are only present in ICU?
    additionalTimeZones = [zone for zone in icuZones if not isIANATimeZone(zone)]
    if additionalTimeZones:
        raise RuntimeError(
            "Additional zones present in ICU, did you forget "
            "to run intl/update-tzdata.sh? %s" % additionalTimeZones
        )

    # Zones which are marked as links in ICU.
    result = ((zone, icuLinks[zone]) for zone in ianaZones if isICULink(zone))

    # Remove unnecessary UTC mappings.
    utcnames = ["Etc/UTC", "Etc/UCT", "Etc/GMT"]
    result = ((zone, target) for (zone, target) in result if zone.name not in utcnames)

    return sorted(result, key=itemgetter(0))


def findIncorrectICULinks(ianaZones, ianaLinks, icuZones, icuLinks):
    """Find incorrect ICU link entries."""

    def isIANATimeZone(zone):
        return zone in ianaZones or zone in ianaLinks

    def isICUTimeZone(zone):
        return zone in icuZones or zone in icuLinks

    def isICULink(zone):
        return zone in icuLinks

    def isICUZone(zone):
        return zone in icuZones

    # All links should be present in ICU.
    missingTimeZones = [zone for zone in ianaLinks.keys() if not isICUTimeZone(zone)]
    if missingTimeZones:
        raise RuntimeError(
            "Not all zones are present in ICU, did you forget "
            "to run intl/update-tzdata.sh? %s" % missingTimeZones
        )

    # Links which are only present in ICU?
    additionalTimeZones = [zone for zone in icuLinks.keys() if not isIANATimeZone(zone)]
    if additionalTimeZones:
        raise RuntimeError(
            "Additional links present in ICU, did you forget "
            "to run intl/update-tzdata.sh? %s" % additionalTimeZones
        )

    result = chain(
        # IANA links which have a different target in ICU.
        (
            (zone, target, icuLinks[zone])
            for (zone, target) in ianaLinks.items()
            if isICULink(zone) and target != icuLinks[zone]
        ),
        # IANA links which are zones in ICU.
        (
            (zone, target, zone.name)
            for (zone, target) in ianaLinks.items()
            if isICUZone(zone)
        ),
    )

    # Remove unnecessary UTC mappings.
    utcnames = ["Etc/UTC", "Etc/UCT", "Etc/GMT"]
    result = (
        (zone, target, icuTarget)
        for (zone, target, icuTarget) in result
        if target not in utcnames or icuTarget not in utcnames
    )

    return sorted(result, key=itemgetter(0))


generatedFileWarning = "// Generated by make_intl_data.py. DO NOT EDIT."
tzdataVersionComment = "// tzdata version = {0}"


def processTimeZones(
    tzdataDir, icuDir, icuTzDir, version, ignoreBackzone, ignoreFactory, out
):
    """Read the time zone info and create a new time zone cpp file."""
    print("Processing tzdata mapping...")
    (ianaZones, ianaLinks) = readIANATimeZones(tzdataDir, ignoreBackzone, ignoreFactory)
    (icuZones, icuLinks) = readICUTimeZones(icuDir, icuTzDir, ignoreFactory)
    (legacyZones, legacyLinks) = readICULegacyZones(icuDir)

    # Remove all legacy ICU time zones.
    icuZones = {zone for zone in icuZones if zone not in legacyZones}
    icuLinks = {
        zone: target for (zone, target) in icuLinks.items() if zone not in legacyLinks
    }

    incorrectZones = findIncorrectICUZones(
        ianaZones, ianaLinks, icuZones, icuLinks, ignoreBackzone
    )
    if not incorrectZones:
        print("<<< No incorrect ICU time zones found, please update Intl.js! >>>")
        print("<<< Maybe https://ssl.icu-project.org/trac/ticket/12044 was fixed? >>>")

    incorrectLinks = findIncorrectICULinks(ianaZones, ianaLinks, icuZones, icuLinks)
    if not incorrectLinks:
        print("<<< No incorrect ICU time zone links found, please update Intl.js! >>>")
        print("<<< Maybe https://ssl.icu-project.org/trac/ticket/12044 was fixed? >>>")

    print("Writing Intl tzdata file...")
    with io.open(out, mode="w", encoding="utf-8", newline="") as f:
        println = partial(print, file=f)

        println(generatedFileWarning)
        println(tzdataVersionComment.format(version))
        println("")

        println("#ifndef builtin_intl_TimeZoneDataGenerated_h")
        println("#define builtin_intl_TimeZoneDataGenerated_h")
        println("")

        println("namespace js {")
        println("namespace timezone {")
        println("")

        println("// Format:")
        println('// "ZoneName" // ICU-Name [time zone file]')
        println("const char* const ianaZonesTreatedAsLinksByICU[] = {")
        for zone, icuZone in incorrectZones:
            println('    "%s", // %s [%s]' % (zone, icuZone, zone.filename))
        println("};")
        println("")

        println("// Format:")
        println('// "LinkName", "Target" // ICU-Target [time zone file]')
        println("struct LinkAndTarget")
        println("{")
        println("    const char* const link;")
        println("    const char* const target;")
        println("};")
        println("")
        println("const LinkAndTarget ianaLinksCanonicalizedDifferentlyByICU[] = {")
        for zone, target, icuTarget in incorrectLinks:
            println(
                '    { "%s", "%s" }, // %s [%s]'
                % (zone, target, icuTarget, zone.filename)
            )
        println("};")
        println("")

        println(
            "// Legacy ICU time zones, these are not valid IANA time zone names. We also"
        )
        println("// disallow the old and deprecated System V time zones.")
        println(
            "// https://ssl.icu-project.org/repos/icu/trunk/icu4c/source/tools/tzcode/icuzones"
        )  # NOQA: E501
        println("const char* const legacyICUTimeZones[] = {")
        for zone in chain(sorted(legacyLinks.keys()), sorted(legacyZones)):
            println('    "%s",' % zone)
        println("};")
        println("")

        println("} // namespace timezone")
        println("} // namespace js")
        println("")
        println("#endif /* builtin_intl_TimeZoneDataGenerated_h */")


def updateBackzoneLinks(tzdataDir, links):
    def withZone(fn):
        return lambda zone_target: fn(zone_target[0])

    (backzoneZones, backzoneLinks) = readIANAFiles(tzdataDir, ["backzone"])
    (stableZones, updatedLinks, updatedZones) = partition(
        links.items(),
        # Link not changed in backzone.
        withZone(lambda zone: zone not in backzoneLinks and zone not in backzoneZones),
        # Link has a new target.
        withZone(lambda zone: zone in backzoneLinks),
    )
    # Keep stable zones and links with updated target.
    return dict(
        chain(
            stableZones,
            map(withZone(lambda zone: (zone, backzoneLinks[zone])), updatedLinks),
        )
    )


def generateTzDataLinkTestContent(testDir, version, fileName, description, links):
    with io.open(
        os.path.join(testDir, fileName), mode="w", encoding="utf-8", newline=""
    ) as f:
        println = partial(print, file=f)

        println('// |reftest| skip-if(!this.hasOwnProperty("Intl"))')
        println("")
        println(generatedFileWarning)
        println(tzdataVersionComment.format(version))
        println(
            """
const tzMapper = [
    x => x,
    x => x.toUpperCase(),
    x => x.toLowerCase(),
];
"""
        )

        println(description)
        println("const links = {")
        for zone, target in sorted(links, key=itemgetter(0)):
            println('    "%s": "%s",' % (zone, target))
        println("};")

        println(
            """
for (let [linkName, target] of Object.entries(links)) {
    if (target === "Etc/UTC" || target === "Etc/GMT")
        target = "UTC";

    for (let map of tzMapper) {
        let dtf = new Intl.DateTimeFormat(undefined, {timeZone: map(linkName)});
        let resolvedTimeZone = dtf.resolvedOptions().timeZone;
        assertEq(resolvedTimeZone, target, `${linkName} -> ${target}`);
    }
}
"""
        )
        println(
            """
if (typeof reportCompare === "function")
    reportCompare(0, 0, "ok");
"""
        )


def generateTzDataTestBackwardLinks(tzdataDir, version, ignoreBackzone, testDir):
    (zones, links) = readIANAFiles(tzdataDir, ["backward"])
    assert len(zones) == 0

    if not ignoreBackzone:
        links = updateBackzoneLinks(tzdataDir, links)

    generateTzDataLinkTestContent(
        testDir,
        version,
        "timeZone_backward_links.js",
        "// Link names derived from IANA Time Zone Database, backward file.",
        links.items(),
    )


def generateTzDataTestNotBackwardLinks(tzdataDir, version, ignoreBackzone, testDir):
    tzfiles = filterfalse(
        {"backward", "backzone"}.__contains__, listIANAFiles(tzdataDir)
    )
    (zones, links) = readIANAFiles(tzdataDir, tzfiles)

    if not ignoreBackzone:
        links = updateBackzoneLinks(tzdataDir, links)

    generateTzDataLinkTestContent(
        testDir,
        version,
        "timeZone_notbackward_links.js",
        "// Link names derived from IANA Time Zone Database, excluding backward file.",
        links.items(),
    )


def generateTzDataTestBackzone(tzdataDir, version, ignoreBackzone, testDir):
    backzoneFiles = {"backzone"}
    (bkfiles, tzfiles) = partition(listIANAFiles(tzdataDir), backzoneFiles.__contains__)

    # Read zone and link infos.
    (zones, links) = readIANAFiles(tzdataDir, tzfiles)
    (backzones, backlinks) = readIANAFiles(tzdataDir, bkfiles)

    if not ignoreBackzone:
        comment = """\
// This file was generated with historical, pre-1970 backzone information
// respected. Therefore, every zone key listed below is its own Zone, not
// a Link to a modern-day target as IANA ignoring backzones would say.

"""
    else:
        comment = """\
// This file was generated while ignoring historical, pre-1970 backzone
// information. Therefore, every zone key listed below is part of a Link
// whose target is the corresponding value.

"""

    generateTzDataLinkTestContent(
        testDir,
        version,
        "timeZone_backzone.js",
        comment + "// Backzone zones derived from IANA Time Zone Database.",
        (
            (zone, zone if not ignoreBackzone else links[zone])
            for zone in backzones
            if zone in links
        ),
    )


def generateTzDataTestBackzoneLinks(tzdataDir, version, ignoreBackzone, testDir):
    backzoneFiles = {"backzone"}
    (bkfiles, tzfiles) = partition(listIANAFiles(tzdataDir), backzoneFiles.__contains__)

    # Read zone and link infos.
    (zones, links) = readIANAFiles(tzdataDir, tzfiles)
    (backzones, backlinks) = readIANAFiles(tzdataDir, bkfiles)

    if not ignoreBackzone:
        comment = """\
// This file was generated with historical, pre-1970 backzone information
// respected. Therefore, every zone key listed below points to a target
// in the backzone file and not to its modern-day target as IANA ignoring
// backzones would say.

"""
    else:
        comment = """\
// This file was generated while ignoring historical, pre-1970 backzone
// information. Therefore, every zone key listed below is part of a Link
// whose target is the corresponding value ignoring any backzone entries.

"""

    generateTzDataLinkTestContent(
        testDir,
        version,
        "timeZone_backzone_links.js",
        comment + "// Backzone links derived from IANA Time Zone Database.",
        (
            (zone, target if not ignoreBackzone else links[zone])
            for (zone, target) in backlinks.items()
        ),
    )


def generateTzDataTestVersion(tzdataDir, version, testDir):
    fileName = "timeZone_version.js"

    with io.open(
        os.path.join(testDir, fileName), mode="w", encoding="utf-8", newline=""
    ) as f:
        println = partial(print, file=f)

        println('// |reftest| skip-if(!this.hasOwnProperty("Intl"))')
        println("")
        println(generatedFileWarning)
        println(tzdataVersionComment.format(version))
        println("""const tzdata = "{0}";""".format(version))

        println(
            """
if (typeof getICUOptions === "undefined") {
    var getICUOptions = SpecialPowers.Cu.getJSTestingFunctions().getICUOptions;
}

var options = getICUOptions();

assertEq(options.tzdata, tzdata);

if (typeof reportCompare === "function")
    reportCompare(0, 0, "ok");
"""
        )


def generateTzDataTestCanonicalZones(
    tzdataDir, version, ignoreBackzone, ignoreFactory, testDir
):
    fileName = "supportedValuesOf-timeZones-canonical.js"

    # Read zone and link infos.
    (ianaZones, _) = readIANATimeZones(tzdataDir, ignoreBackzone, ignoreFactory)

    # Replace Etc/GMT and Etc/UTC with UTC.
    ianaZones.remove(Zone("Etc/GMT"))
    ianaZones.remove(Zone("Etc/UTC"))
    ianaZones.add(Zone("UTC"))

    # See findIncorrectICUZones() for why Asia/Hanoi has to be special-cased.
    ianaZones.remove(Zone("Asia/Hanoi"))

    if not ignoreBackzone:
        comment = """\
// This file was generated with historical, pre-1970 backzone information
// respected.
"""
    else:
        comment = """\
// This file was generated while ignoring historical, pre-1970 backzone
// information.
"""

    with io.open(
        os.path.join(testDir, fileName), mode="w", encoding="utf-8", newline=""
    ) as f:
        println = partial(print, file=f)

        println('// |reftest| skip-if(!this.hasOwnProperty("Intl"))')
        println("")
        println(generatedFileWarning)
        println(tzdataVersionComment.format(version))
        println("")
        println(comment)

        println("const zones = [")
        for zone in sorted(ianaZones):
            println(f'  "{zone}",')
        println("];")

        println(
            """
let supported = Intl.supportedValuesOf("timeZone");

assertEqArray(supported, zones);

if (typeof reportCompare === "function")
    reportCompare(0, 0, "ok");
"""
        )


def generateTzDataTests(tzdataDir, version, ignoreBackzone, ignoreFactory, testDir):
    dtfTestDir = os.path.join(testDir, "DateTimeFormat")
    if not os.path.isdir(dtfTestDir):
        raise RuntimeError("not a directory: %s" % dtfTestDir)

    generateTzDataTestBackwardLinks(tzdataDir, version, ignoreBackzone, dtfTestDir)
    generateTzDataTestNotBackwardLinks(tzdataDir, version, ignoreBackzone, dtfTestDir)
    generateTzDataTestBackzone(tzdataDir, version, ignoreBackzone, dtfTestDir)
    generateTzDataTestBackzoneLinks(tzdataDir, version, ignoreBackzone, dtfTestDir)
    generateTzDataTestVersion(tzdataDir, version, dtfTestDir)
    generateTzDataTestCanonicalZones(
        tzdataDir, version, ignoreBackzone, ignoreFactory, testDir
    )


def updateTzdata(topsrcdir, args):
    """Update the time zone cpp file."""

    icuDir = os.path.join(topsrcdir, "intl/icu/source")
    if not os.path.isdir(icuDir):
        raise RuntimeError("not a directory: %s" % icuDir)

    icuTzDir = os.path.join(topsrcdir, "intl/tzdata/source")
    if not os.path.isdir(icuTzDir):
        raise RuntimeError("not a directory: %s" % icuTzDir)

    intlTestDir = os.path.join(topsrcdir, "js/src/tests/non262/Intl")
    if not os.path.isdir(intlTestDir):
        raise RuntimeError("not a directory: %s" % intlTestDir)

    tzDir = args.tz
    if tzDir is not None and not (os.path.isdir(tzDir) or os.path.isfile(tzDir)):
        raise RuntimeError("not a directory or file: %s" % tzDir)
    ignoreBackzone = args.ignore_backzone
    # TODO: Accept or ignore the placeholder time zone "Factory"?
    ignoreFactory = False
    out = args.out

    version = icuTzDataVersion(icuTzDir)
    url = (
        "https://www.iana.org/time-zones/repository/releases/tzdata%s.tar.gz" % version
    )

    print("Arguments:")
    print("\ttzdata version: %s" % version)
    print("\ttzdata URL: %s" % url)
    print("\ttzdata directory|file: %s" % tzDir)
    print("\tICU directory: %s" % icuDir)
    print("\tICU timezone directory: %s" % icuTzDir)
    print("\tIgnore backzone file: %s" % ignoreBackzone)
    print("\tOutput file: %s" % out)
    print("")

    def updateFrom(f):
        if os.path.isfile(f) and tarfile.is_tarfile(f):
            with tarfile.open(f, "r:*") as tar:
                processTimeZones(
                    TzDataFile(tar),
                    icuDir,
                    icuTzDir,
                    version,
                    ignoreBackzone,
                    ignoreFactory,
                    out,
                )
                generateTzDataTests(
                    TzDataFile(tar), version, ignoreBackzone, ignoreFactory, intlTestDir
                )
        elif os.path.isdir(f):
            processTimeZones(
                TzDataDir(f),
                icuDir,
                icuTzDir,
                version,
                ignoreBackzone,
                ignoreFactory,
                out,
            )
            generateTzDataTests(
                TzDataDir(f), version, ignoreBackzone, ignoreFactory, intlTestDir
            )
        else:
            raise RuntimeError("unknown format")

    if tzDir is None:
        print("Downloading tzdata file...")
        with closing(urlopen(url)) as tzfile:
            fname = urlsplit(tzfile.geturl()).path.split("/")[-1]
            with tempfile.NamedTemporaryFile(suffix=fname) as tztmpfile:
                print("File stored in %s" % tztmpfile.name)
                tztmpfile.write(tzfile.read())
                tztmpfile.flush()
                updateFrom(tztmpfile.name)
    else:
        updateFrom(tzDir)


def readCurrencyFile(tree):
    reCurrency = re.compile(r"^[A-Z]{3}$")
    reIntMinorUnits = re.compile(r"^\d+$")

    for country in tree.iterfind(".//CcyNtry"):
        # Skip entry if no currency information is available.
        currency = country.findtext("Ccy")
        if currency is None:
            continue
        assert reCurrency.match(currency)

        minorUnits = country.findtext("CcyMnrUnts")
        assert minorUnits is not None

        # Skip all entries without minorUnits or which use the default minorUnits.
        if reIntMinorUnits.match(minorUnits) and int(minorUnits) != 2:
            currencyName = country.findtext("CcyNm")
            countryName = country.findtext("CtryNm")
            yield (currency, int(minorUnits), currencyName, countryName)


def writeCurrencyFile(published, currencies, out):
    with io.open(out, mode="w", encoding="utf-8", newline="") as f:
        println = partial(print, file=f)

        println(generatedFileWarning)
        println("// Version: {}".format(published))

        println(
            """
/**
 * Mapping from currency codes to the number of decimal digits used for them.
 * Default is 2 digits.
 *
 * Spec: ISO 4217 Currency and Funds Code List.
 * http://www.currency-iso.org/en/home/tables/table-a1.html
 */"""
        )
        println("var currencyDigits = {")
        for currency, entries in groupby(
            sorted(currencies, key=itemgetter(0)), itemgetter(0)
        ):
            for _, minorUnits, currencyName, countryName in entries:
                println("  // {} ({})".format(currencyName, countryName))
            println("  {}: {},".format(currency, minorUnits))
        println("};")


def updateCurrency(topsrcdir, args):
    """Update the CurrencyDataGenerated.js file."""
    import xml.etree.ElementTree as ET
    from random import randint

    url = args.url
    out = args.out
    filename = args.file

    print("Arguments:")
    print("\tDownload url: %s" % url)
    print("\tLocal currency file: %s" % filename)
    print("\tOutput file: %s" % out)
    print("")

    def updateFrom(currencyFile):
        print("Processing currency code list file...")
        tree = ET.parse(currencyFile)
        published = tree.getroot().attrib["Pblshd"]
        currencies = readCurrencyFile(tree)

        print("Writing CurrencyData file...")
        writeCurrencyFile(published, currencies, out)

    if filename is not None:
        print("Always make sure you have the newest currency code list file!")
        updateFrom(filename)
    else:
        print("Downloading currency & funds code list...")
        request = UrlRequest(url)
        request.add_header(
            "User-agent",
            "Mozilla/5.0 (Mobile; rv:{0}.0) Gecko/{0}.0 Firefox/{0}.0".format(
                randint(1, 999)
            ),
        )
        with closing(urlopen(request)) as currencyFile:
            fname = urlsplit(currencyFile.geturl()).path.split("/")[-1]
            with tempfile.NamedTemporaryFile(suffix=fname) as currencyTmpFile:
                print("File stored in %s" % currencyTmpFile.name)
                currencyTmpFile.write(currencyFile.read())
                currencyTmpFile.flush()
                updateFrom(currencyTmpFile.name)


def writeUnicodeExtensionsMappings(println, mapping, extension):
    println(
        """
template <size_t Length>
static inline bool Is{0}Key(mozilla::Span<const char> key, const char (&str)[Length]) {{
  static_assert(Length == {0}KeyLength + 1,
                "{0} extension key is two characters long");
  return memcmp(key.data(), str, Length - 1) == 0;
}}

template <size_t Length>
static inline bool Is{0}Type(mozilla::Span<const char> type, const char (&str)[Length]) {{
  static_assert(Length > {0}KeyLength + 1,
                "{0} extension type contains more than two characters");
  return type.size() == (Length - 1) &&
         memcmp(type.data(), str, Length - 1) == 0;
}}
""".format(
            extension
        ).rstrip(
            "\n"
        )
    )

    linear_search_max_length = 4

    needs_binary_search = any(
        len(replacements.items()) > linear_search_max_length
        for replacements in mapping.values()
    )

    if needs_binary_search:
        println(
            """
static int32_t Compare{0}Type(const char* a, mozilla::Span<const char> b) {{
  MOZ_ASSERT(!std::char_traits<char>::find(b.data(), b.size(), '\\0'),
             "unexpected null-character in string");

  using UnsignedChar = unsigned char;
  for (size_t i = 0; i < b.size(); i++) {{
    // |a| is zero-terminated and |b| doesn't contain a null-terminator. So if
    // we've reached the end of |a|, the below if-statement will always be true.
    // That ensures we don't read past the end of |a|.
    if (int32_t r = UnsignedChar(a[i]) - UnsignedChar(b[i])) {{
      return r;
    }}
  }}

  // Return zero if both strings are equal or a positive number if |b| is a
  // prefix of |a|.
  return int32_t(UnsignedChar(a[b.size()]));
}}

template <size_t Length>
static inline const char* Search{0}Replacement(
  const char* (&types)[Length], const char* (&aliases)[Length],
  mozilla::Span<const char> type) {{

  auto p = std::lower_bound(std::begin(types), std::end(types), type,
                            [](const auto& a, const auto& b) {{
                              return Compare{0}Type(a, b) < 0;
                            }});
  if (p != std::end(types) && Compare{0}Type(*p, type) == 0) {{
    return aliases[std::distance(std::begin(types), p)];
  }}
  return nullptr;
}}
""".format(
                extension
            ).rstrip(
                "\n"
            )
        )

    println(
        """
/**
 * Mapping from deprecated BCP 47 {0} extension types to their preferred
 * values.
 *
 * Spec: https://www.unicode.org/reports/tr35/#Unicode_Locale_Extension_Data_Files
 * Spec: https://www.unicode.org/reports/tr35/#t_Extension
 */
const char* mozilla::intl::Locale::Replace{0}ExtensionType(
    mozilla::Span<const char> key, mozilla::Span<const char> type) {{
  MOZ_ASSERT(key.size() == {0}KeyLength);
  MOZ_ASSERT(IsCanonicallyCased{0}Key(key));

  MOZ_ASSERT(type.size() > {0}KeyLength);
  MOZ_ASSERT(IsCanonicallyCased{0}Type(type));
""".format(
            extension
        )
    )

    def to_hash_key(replacements):
        return str(sorted(replacements.items()))

    def write_array(subtags, name, length):
        max_entries = (80 - len("    ")) // (length + len('"", '))

        println("    static const char* {}[{}] = {{".format(name, len(subtags)))

        for entries in grouper(subtags, max_entries):
            entries = (
                '"{}"'.format(tag).center(length + 2)
                for tag in entries
                if tag is not None
            )
            println("        {},".format(", ".join(entries)))

        println("    };")

    # Merge duplicate keys.
    key_aliases = {}
    for key, replacements in sorted(mapping.items(), key=itemgetter(0)):
        hash_key = to_hash_key(replacements)
        if hash_key not in key_aliases:
            key_aliases[hash_key] = []
        else:
            key_aliases[hash_key].append(key)

    first_key = True
    for key, replacements in sorted(mapping.items(), key=itemgetter(0)):
        hash_key = to_hash_key(replacements)
        if key in key_aliases[hash_key]:
            continue

        cond = (
            'Is{}Key(key, "{}")'.format(extension, k)
            for k in [key] + key_aliases[hash_key]
        )

        if_kind = "if" if first_key else "else if"
        cond = (" ||\n" + " " * (2 + len(if_kind) + 2)).join(cond)
        println(
            """
  {} ({}) {{""".format(
                if_kind, cond
            ).strip(
                "\n"
            )
        )
        first_key = False

        replacements = sorted(replacements.items(), key=itemgetter(0))

        if len(replacements) > linear_search_max_length:
            types = [t for (t, _) in replacements]
            preferred = [r for (_, r) in replacements]
            max_len = max(len(k) for k in types + preferred)

            write_array(types, "types", max_len)
            write_array(preferred, "aliases", max_len)
            println(
                """
    return Search{}Replacement(types, aliases, type);
""".format(
                    extension
                ).strip(
                    "\n"
                )
            )
        else:
            for type, replacement in replacements:
                println(
                    """
    if (Is{}Type(type, "{}")) {{
      return "{}";
    }}""".format(
                        extension, type, replacement
                    ).strip(
                        "\n"
                    )
                )

        println(
            """
  }""".lstrip(
                "\n"
            )
        )

    println(
        """
  return nullptr;
}
""".strip(
            "\n"
        )
    )


def readICUUnitResourceFile(filepath):
    """Return a set of unit descriptor pairs where the first entry denotes the unit type and the
    second entry the unit name.

    Example:

    root{
        units{
            compound{
            }
            coordinate{
            }
            length{
                meter{
                }
            }
        }
        unitsNarrow:alias{"/LOCALE/unitsShort"}
        unitsShort{
            duration{
                day{
                }
                day-person:alias{"/LOCALE/unitsShort/duration/day"}
            }
            length{
                meter{
                }
            }
        }
    }

    Returns {("length", "meter"), ("duration", "day"), ("duration", "day-person")}
    """

    start_table_re = re.compile(r"^([\w\-%:\"]+)\{$")
    end_table_re = re.compile(r"^\}$")
    table_entry_re = re.compile(r"^([\w\-%:\"]+)\{\"(.*?)\"\}$")

    # The current resource table.
    table = {}

    # List of parent tables when parsing.
    parents = []

    # Track multi-line comments state.
    in_multiline_comment = False

    for line in flines(filepath, "utf-8-sig"):
        # Remove leading and trailing whitespace.
        line = line.strip()

        # Skip over comments.
        if in_multiline_comment:
            if line.endswith("*/"):
                in_multiline_comment = False
            continue

        if line.startswith("//"):
            continue

        if line.startswith("/*"):
            in_multiline_comment = True
            continue

        # Try to match the start of a table, e.g. `length{` or `meter{`.
        match = start_table_re.match(line)
        if match:
            parents.append(table)
            table_name = match.group(1)
            new_table = {}
            table[table_name] = new_table
            table = new_table
            continue

        # Try to match the end of a table.
        match = end_table_re.match(line)
        if match:
            table = parents.pop()
            continue

        # Try to match a table entry, e.g. `dnam{"meter"}`.
        match = table_entry_re.match(line)
        if match:
            entry_key = match.group(1)
            entry_value = match.group(2)
            table[entry_key] = entry_value
            continue

        raise Exception("unexpected line: '{}' in {}".format(line, filepath))

    assert len(parents) == 0, "Not all tables closed"
    assert len(table) == 1, "More than one root table"

    # Remove the top-level language identifier table.
    (_, unit_table) = table.popitem()

    # Add all units for the three display formats "units", "unitsNarrow", and "unitsShort".
    # But exclude the pseudo-units "compound" and "ccoordinate".
    return {
        (unit_type, unit_name if not unit_name.endswith(":alias") else unit_name[:-6])
        for unit_display in ("units", "unitsNarrow", "unitsShort")
        if unit_display in unit_table
        for (unit_type, unit_names) in unit_table[unit_display].items()
        if unit_type != "compound" and unit_type != "coordinate"
        for unit_name in unit_names.keys()
    }


def computeSupportedUnits(all_units, sanctioned_units):
    """Given the set of all possible ICU unit identifiers and the set of sanctioned unit
    identifiers, compute the set of effectively supported ICU unit identifiers.
    """

    def find_match(unit):
        unit_match = [
            (unit_type, unit_name)
            for (unit_type, unit_name) in all_units
            if unit_name == unit
        ]
        if unit_match:
            assert len(unit_match) == 1
            return unit_match[0]
        return None

    def compound_unit_identifiers():
        for numerator in sanctioned_units:
            for denominator in sanctioned_units:
                yield "{}-per-{}".format(numerator, denominator)

    supported_simple_units = {find_match(unit) for unit in sanctioned_units}
    assert None not in supported_simple_units

    supported_compound_units = {
        unit_match
        for unit_match in (find_match(unit) for unit in compound_unit_identifiers())
        if unit_match
    }

    return supported_simple_units | supported_compound_units


def readICUDataFilterForUnits(data_filter_file):
    with io.open(data_filter_file, mode="r", encoding="utf-8") as f:
        data_filter = json.load(f)

    # Find the rule set for the "unit_tree".
    unit_tree_rules = [
        entry["rules"]
        for entry in data_filter["resourceFilters"]
        if entry["categories"] == ["unit_tree"]
    ]
    assert len(unit_tree_rules) == 1

    # Compute the list of included units from that rule set. The regular expression must match
    # "+/*/length/meter" and mustn't match either "-/*" or "+/*/compound".
    included_unit_re = re.compile(r"^\+/\*/(.+?)/(.+)$")
    filtered_units = (included_unit_re.match(unit) for unit in unit_tree_rules[0])

    return {(unit.group(1), unit.group(2)) for unit in filtered_units if unit}


def writeSanctionedSimpleUnitIdentifiersFiles(all_units, sanctioned_units):
    js_src_builtin_intl_dir = os.path.dirname(os.path.abspath(__file__))
    intl_components_src_dir = os.path.join(
        js_src_builtin_intl_dir, "../../../../intl/components/src"
    )

    def find_unit_type(unit):
        result = [
            unit_type for (unit_type, unit_name) in all_units if unit_name == unit
        ]
        assert result and len(result) == 1
        return result[0]

    sanctioned_js_file = os.path.join(
        js_src_builtin_intl_dir, "SanctionedSimpleUnitIdentifiersGenerated.js"
    )
    with io.open(sanctioned_js_file, mode="w", encoding="utf-8", newline="") as f:
        println = partial(print, file=f)

        sanctioned_units_object = json.dumps(
            {unit: True for unit in sorted(sanctioned_units)},
            sort_keys=True,
            indent=2,
            separators=(",", ": "),
        )

        println(generatedFileWarning)

        println(
            """
/**
 * The list of currently supported simple unit identifiers.
 *
 * Intl.NumberFormat Unified API Proposal
 */"""
        )

        println("// prettier-ignore")
        println(
            "var sanctionedSimpleUnitIdentifiers = {};".format(sanctioned_units_object)
        )

    sanctioned_h_file = os.path.join(intl_components_src_dir, "MeasureUnitGenerated.h")
    with io.open(sanctioned_h_file, mode="w", encoding="utf-8", newline="") as f:
        println = partial(print, file=f)

        println(generatedFileWarning)

        println(
            """
#ifndef intl_components_MeasureUnitGenerated_h
#define intl_components_MeasureUnitGenerated_h

namespace mozilla::intl {

struct SimpleMeasureUnit {
  const char* const type;
  const char* const name;
};

/**
 * The list of currently supported simple unit identifiers.
 *
 * The list must be kept in alphabetical order of |name|.
 */
inline constexpr SimpleMeasureUnit simpleMeasureUnits[] = {
    // clang-format off"""
        )

        for unit_name in sorted(sanctioned_units):
            println('  {{"{}", "{}"}},'.format(find_unit_type(unit_name), unit_name))

        println(
            """
    // clang-format on
};

}  // namespace mozilla::intl

#endif
""".strip(
                "\n"
            )
        )

    writeUnitTestFiles(all_units, sanctioned_units)


def writeUnitTestFiles(all_units, sanctioned_units):
    """Generate test files for unit number formatters."""

    js_src_builtin_intl_dir = os.path.dirname(os.path.abspath(__file__))
    test_dir = os.path.join(
        js_src_builtin_intl_dir, "../../tests/non262/Intl/NumberFormat"
    )

    def write_test(file_name, test_content, indent=4):
        file_path = os.path.join(test_dir, file_name)
        with io.open(file_path, mode="w", encoding="utf-8", newline="") as f:
            println = partial(print, file=f)

            println('// |reftest| skip-if(!this.hasOwnProperty("Intl"))')
            println("")
            println(generatedFileWarning)
            println("")

            sanctioned_units_array = json.dumps(
                [unit for unit in sorted(sanctioned_units)],
                indent=indent,
                separators=(",", ": "),
            )

            println(
                "const sanctionedSimpleUnitIdentifiers = {};".format(
                    sanctioned_units_array
                )
            )

            println(test_content)

            println(
                """
if (typeof reportCompare === "function")
{}reportCompare(true, true);""".format(
                    " " * indent
                )
            )

    write_test(
        "unit-compound-combinations.js",
        """
// Test all simple unit identifier combinations are allowed.

for (const numerator of sanctionedSimpleUnitIdentifiers) {
    for (const denominator of sanctionedSimpleUnitIdentifiers) {
        const unit = `${numerator}-per-${denominator}`;
        const nf = new Intl.NumberFormat("en", {style: "unit", unit});

        assertEq(nf.format(1), nf.formatToParts(1).map(p => p.value).join(""));
    }
}""",
    )

    all_units_array = json.dumps(
        ["-".join(unit) for unit in sorted(all_units)], indent=4, separators=(",", ": ")
    )

    write_test(
        "unit-well-formed.js",
        """
const allUnits = {};
""".format(
            all_units_array
        )
        + r"""
// Test only sanctioned unit identifiers are allowed.

for (const typeAndUnit of allUnits) {
    const [_, type, unit] = typeAndUnit.match(/(\w+)-(.+)/);

    let allowed;
    if (unit.includes("-per-")) {
        const [numerator, denominator] = unit.split("-per-");
        allowed = sanctionedSimpleUnitIdentifiers.includes(numerator) &&
                  sanctionedSimpleUnitIdentifiers.includes(denominator);
    } else {
        allowed = sanctionedSimpleUnitIdentifiers.includes(unit);
    }

    if (allowed) {
        const nf = new Intl.NumberFormat("en", {style: "unit", unit});
        assertEq(nf.format(1), nf.formatToParts(1).map(p => p.value).join(""));
    } else {
        assertThrowsInstanceOf(() => new Intl.NumberFormat("en", {style: "unit", unit}),
                               RangeError, `Missing error for "${typeAndUnit}"`);
    }
}""",
    )

    write_test(
        "unit-formatToParts-has-unit-field.js",
        """
// Test only English and Chinese to keep the overall runtime reasonable.
//
// Chinese is included because it contains more than one "unit" element for
// certain unit combinations.
const locales = ["en", "zh"];

// Plural rules for English only differentiate between "one" and "other". Plural
// rules for Chinese only use "other". That means we only need to test two values
// per unit.
const values = [0, 1];

// Ensure unit formatters contain at least one "unit" element.

for (const locale of locales) {
  for (const unit of sanctionedSimpleUnitIdentifiers) {
    const nf = new Intl.NumberFormat(locale, {style: "unit", unit});

    for (const value of values) {
      assertEq(nf.formatToParts(value).some(e => e.type === "unit"), true,
               `locale=${locale}, unit=${unit}`);
    }
  }

  for (const numerator of sanctionedSimpleUnitIdentifiers) {
    for (const denominator of sanctionedSimpleUnitIdentifiers) {
      const unit = `${numerator}-per-${denominator}`;
      const nf = new Intl.NumberFormat(locale, {style: "unit", unit});

      for (const value of values) {
        assertEq(nf.formatToParts(value).some(e => e.type === "unit"), true,
                 `locale=${locale}, unit=${unit}`);
      }
    }
  }
}""",
        indent=2,
    )


def updateUnits(topsrcdir, args):
    js_src_builtin_intl_dir = os.path.dirname(os.path.abspath(__file__))
    icu_path = os.path.join(topsrcdir, "intl", "icu")
    icu_unit_path = os.path.join(icu_path, "source", "data", "unit")

    with io.open(
        os.path.join(js_src_builtin_intl_dir, "SanctionedSimpleUnitIdentifiers.yaml"),
        mode="r",
        encoding="utf-8",
    ) as f:
        sanctioned_units = yaml.safe_load(f)

    # Read all possible ICU unit identifiers from the "unit/root.txt" resource.
    unit_root_file = os.path.join(icu_unit_path, "root.txt")
    all_units = readICUUnitResourceFile(unit_root_file)

    # Compute the set of effectively supported ICU unit identifiers.
    supported_units = computeSupportedUnits(all_units, sanctioned_units)

    # Read the list of units we're including into the ICU data file.
    data_filter_file = os.path.join(icu_path, "data_filter.json")
    filtered_units = readICUDataFilterForUnits(data_filter_file)

    # Both sets must match to avoid resource loading errors at runtime.
    if supported_units != filtered_units:

        def units_to_string(units):
            return ", ".join("/".join(u) for u in units)

        missing = supported_units - filtered_units
        if missing:
            raise RuntimeError("Missing units: {}".format(units_to_string(missing)))

        # Not exactly an error, but we currently don't have a use case where we need to support
        # more units than required by ECMA-402.
        extra = filtered_units - supported_units
        if extra:
            raise RuntimeError("Unnecessary units: {}".format(units_to_string(extra)))

    writeSanctionedSimpleUnitIdentifiersFiles(all_units, sanctioned_units)


def readICUNumberingSystemsResourceFile(filepath):
    """Returns a dictionary of numbering systems where the key denotes the numbering system name
    and the value a dictionary with additional numbering system data.

    Example:

    numberingSystems:table(nofallback){
        numberingSystems{
            latn{
                algorithmic:int{0}
                desc{"0123456789"}
                radix:int{10}
            }
            roman{
                algorithmic:int{1}
                desc{"%roman-upper"}
                radix:int{10}
            }
        }
    }

    Returns {"latn": {"digits": "0123456789", "algorithmic": False},
             "roman": {"algorithmic": True}}
    """

    start_table_re = re.compile(r"^(\w+)(?:\:[\w\(\)]+)?\{$")
    end_table_re = re.compile(r"^\}$")
    table_entry_re = re.compile(r"^(\w+)(?:\:[\w\(\)]+)?\{(?:(?:\"(.*?)\")|(\d+))\}$")

    # The current resource table.
    table = {}

    # List of parent tables when parsing.
    parents = []

    # Track multi-line comments state.
    in_multiline_comment = False

    for line in flines(filepath, "utf-8-sig"):
        # Remove leading and trailing whitespace.
        line = line.strip()

        # Skip over comments.
        if in_multiline_comment:
            if line.endswith("*/"):
                in_multiline_comment = False
            continue

        if line.startswith("//"):
            continue

        if line.startswith("/*"):
            in_multiline_comment = True
            continue

        # Try to match the start of a table, e.g. `latn{`.
        match = start_table_re.match(line)
        if match:
            parents.append(table)
            table_name = match.group(1)
            new_table = {}
            table[table_name] = new_table
            table = new_table
            continue

        # Try to match the end of a table.
        match = end_table_re.match(line)
        if match:
            table = parents.pop()
            continue

        # Try to match a table entry, e.g. `desc{"0123456789"}`.
        match = table_entry_re.match(line)
        if match:
            entry_key = match.group(1)
            entry_value = (
                match.group(2) if match.group(2) is not None else int(match.group(3))
            )
            table[entry_key] = entry_value
            continue

        raise Exception("unexpected line: '{}' in {}".format(line, filepath))

    assert len(parents) == 0, "Not all tables closed"
    assert len(table) == 1, "More than one root table"

    # Remove the two top-level "numberingSystems" tables.
    (_, numbering_systems) = table.popitem()
    (_, numbering_systems) = numbering_systems.popitem()

    # Assert all numbering systems use base 10.
    assert all(ns["radix"] == 10 for ns in numbering_systems.values())

    # Return the numbering systems.
    return {
        key: {"digits": value["desc"], "algorithmic": False}
        if not bool(value["algorithmic"])
        else {"algorithmic": True}
        for (key, value) in numbering_systems.items()
    }


def writeNumberingSystemFiles(numbering_systems):
    js_src_builtin_intl_dir = os.path.dirname(os.path.abspath(__file__))

    numbering_systems_js_file = os.path.join(
        js_src_builtin_intl_dir, "NumberingSystemsGenerated.h"
    )
    with io.open(
        numbering_systems_js_file, mode="w", encoding="utf-8", newline=""
    ) as f:
        println = partial(print, file=f)

        println(generatedFileWarning)

        println(
            """
/**
 * The list of numbering systems with simple digit mappings.
 */

#ifndef builtin_intl_NumberingSystemsGenerated_h
#define builtin_intl_NumberingSystemsGenerated_h
"""
        )

        simple_numbering_systems = sorted(
            name
            for (name, value) in numbering_systems.items()
            if not value["algorithmic"]
        )

        println("// clang-format off")
        println("#define NUMBERING_SYSTEMS_WITH_SIMPLE_DIGIT_MAPPINGS \\")
        println(
            "{}".format(
                ", \\\n".join(
                    '  "{}"'.format(name) for name in simple_numbering_systems
                )
            )
        )
        println("// clang-format on")
        println("")

        println("#endif  // builtin_intl_NumberingSystemsGenerated_h")

    js_src_builtin_intl_dir = os.path.dirname(os.path.abspath(__file__))
    test_dir = os.path.join(js_src_builtin_intl_dir, "../../tests/non262/Intl")

    intl_shell_js_file = os.path.join(test_dir, "shell.js")

    with io.open(intl_shell_js_file, mode="w", encoding="utf-8", newline="") as f:
        println = partial(print, file=f)

        println(generatedFileWarning)

        println(
            """
// source: CLDR file common/bcp47/number.xml; version CLDR {}.
// https://github.com/unicode-org/cldr/blob/master/common/bcp47/number.xml
// https://github.com/unicode-org/cldr/blob/master/common/supplemental/numberingSystems.xml
""".format(
                readCLDRVersionFromICU()
            ).rstrip()
        )

        numbering_systems_object = json.dumps(
            numbering_systems,
            indent=2,
            separators=(",", ": "),
            sort_keys=True,
            ensure_ascii=False,
        )
        println("const numberingSystems = {};".format(numbering_systems_object))


def updateNumberingSystems(topsrcdir, args):
    js_src_builtin_intl_dir = os.path.dirname(os.path.abspath(__file__))
    icu_path = os.path.join(topsrcdir, "intl", "icu")
    icu_misc_path = os.path.join(icu_path, "source", "data", "misc")

    with io.open(
        os.path.join(js_src_builtin_intl_dir, "NumberingSystems.yaml"),
        mode="r",
        encoding="utf-8",
    ) as f:
        numbering_systems = yaml.safe_load(f)

    # Read all possible ICU unit identifiers from the "misc/numberingSystems.txt" resource.
    misc_ns_file = os.path.join(icu_misc_path, "numberingSystems.txt")
    all_numbering_systems = readICUNumberingSystemsResourceFile(misc_ns_file)

    all_numbering_systems_simple_digits = {
        name
        for (name, value) in all_numbering_systems.items()
        if not value["algorithmic"]
    }

    # Assert ICU includes support for all required numbering systems. If this assertion fails,
    # something is broken in ICU.
    assert all_numbering_systems_simple_digits.issuperset(
        numbering_systems
    ), "{}".format(numbering_systems.difference(all_numbering_systems_simple_digits))

    # Assert the spec requires support for all numbering systems with simple digit mappings. If
    # this assertion fails, file a PR at <https://github.com/tc39/ecma402> to include any new
    # numbering systems.
    assert all_numbering_systems_simple_digits.issubset(numbering_systems), "{}".format(
        all_numbering_systems_simple_digits.difference(numbering_systems)
    )

    writeNumberingSystemFiles(all_numbering_systems)


if __name__ == "__main__":
    import argparse

    # This script must reside in js/src/builtin/intl to work correctly.
    (thisDir, thisFile) = os.path.split(os.path.abspath(__file__))
    dirPaths = os.path.normpath(thisDir).split(os.sep)
    if "/".join(dirPaths[-4:]) != "js/src/builtin/intl":
        raise RuntimeError("%s must reside in js/src/builtin/intl" % __file__)
    topsrcdir = "/".join(dirPaths[:-4])

    def EnsureHttps(v):
        if not v.startswith("https:"):
            raise argparse.ArgumentTypeError("URL protocol must be https: " % v)
        return v

    parser = argparse.ArgumentParser(description="Update intl data.")
    subparsers = parser.add_subparsers(help="Select update mode")

    parser_cldr_tags = subparsers.add_parser(
        "langtags", help="Update CLDR language tags data"
    )
    parser_cldr_tags.add_argument(
        "--version", metavar="VERSION", help="CLDR version number"
    )
    parser_cldr_tags.add_argument(
        "--url",
        metavar="URL",
        default="https://unicode.org/Public/cldr/<VERSION>/cldr-common-<VERSION>.0.zip",
        type=EnsureHttps,
        help="Download url CLDR data (default: %(default)s)",
    )
    parser_cldr_tags.add_argument(
        "--out",
        default=os.path.join(
            topsrcdir, "intl", "components", "src", "LocaleGenerated.cpp"
        ),
        help="Output file (default: %(default)s)",
    )
    parser_cldr_tags.add_argument(
        "file", nargs="?", help="Local cldr-common.zip file, if omitted uses <URL>"
    )
    parser_cldr_tags.set_defaults(func=updateCLDRLangTags)

    parser_tz = subparsers.add_parser("tzdata", help="Update tzdata")
    parser_tz.add_argument(
        "--tz",
        help="Local tzdata directory or file, if omitted downloads tzdata "
        "distribution from https://www.iana.org/time-zones/",
    )
    # ICU doesn't include the backzone file by default, but we still like to
    # use the backzone time zone names to avoid user confusion. This does lead
    # to formatting "historic" dates (pre-1970 era) with the wrong time zone,
    # but that's probably acceptable for now.
    parser_tz.add_argument(
        "--ignore-backzone",
        action="store_true",
        help="Ignore tzdata's 'backzone' file. Can be enabled to generate more "
        "accurate time zone canonicalization reflecting the actual time "
        "zones as used by ICU.",
    )
    parser_tz.add_argument(
        "--out",
        default=os.path.join(thisDir, "TimeZoneDataGenerated.h"),
        help="Output file (default: %(default)s)",
    )
    parser_tz.set_defaults(func=partial(updateTzdata, topsrcdir))

    parser_currency = subparsers.add_parser(
        "currency", help="Update currency digits mapping"
    )
    parser_currency.add_argument(
        "--url",
        metavar="URL",
        default="https://www.six-group.com/dam/download/financial-information/data-center/iso-currrency/lists/list-one.xml",  # NOQA: E501
        type=EnsureHttps,
        help="Download url for the currency & funds code list (default: "
        "%(default)s)",
    )
    parser_currency.add_argument(
        "--out",
        default=os.path.join(thisDir, "CurrencyDataGenerated.js"),
        help="Output file (default: %(default)s)",
    )
    parser_currency.add_argument(
        "file", nargs="?", help="Local currency code list file, if omitted uses <URL>"
    )
    parser_currency.set_defaults(func=partial(updateCurrency, topsrcdir))

    parser_units = subparsers.add_parser(
        "units", help="Update sanctioned unit identifiers mapping"
    )
    parser_units.set_defaults(func=partial(updateUnits, topsrcdir))

    parser_numbering_systems = subparsers.add_parser(
        "numbering", help="Update numbering systems with simple digit mappings"
    )
    parser_numbering_systems.set_defaults(
        func=partial(updateNumberingSystems, topsrcdir)
    )

    args = parser.parse_args()
    args.func(args)
