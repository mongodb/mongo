# Copyright (C) 2018 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html

import io as pyio
import json
import os
import unittest

from .. import InFile
from ..comment_stripper import CommentStripper
from ..filtration import Filter

EXAMPLE_FILE_STEMS = [
    "af_NA",
    "af_VARIANT",
    "af_ZA_VARIANT",
    "af_ZA",
    "af",
    "ar",
    "ar_SA",
    "ars",
    "bs_BA",
    "bs_Cyrl_BA",
    "bs_Cyrl",
    "bs_Latn_BA",
    "bs_Latn",
    "bs",
    "en_001",
    "en_150",
    "en_DE",
    "en_GB",
    "en_US",
    "root",
    "sr_BA",
    "sr_CS",
    "sr_Cyrl_BA",
    "sr_Cyrl_CS",
    "sr_Cyrl_ME",
    "sr_Cyrl",
    "sr_Latn_BA",
    "sr_Latn_CS",
    "sr_Latn_ME_VARIANT",
    "sr_Latn_ME",
    "sr_Latn",
    "sr_ME",
    "sr",
    "vai_Latn_LR",
    "vai_Latn",
    "vai_LR",
    "vai_Vaii_LR",
    "vai_Vaii",
    "vai",
    "yue",
    "zh_CN",
    "zh_Hans_CN",
    "zh_Hans_HK",
    "zh_Hans_MO",
    "zh_Hans_SG",
    "zh_Hans",
    "zh_Hant_HK",
    "zh_Hant_MO",
    "zh_Hant_TW",
    "zh_Hant",
    "zh_HK",
    "zh_MO",
    "zh_SG",
    "zh_TW",
    "zh"
]


class TestIO(object):
    def __init__(self):
        pass

    def read_locale_deps(self, tree):
        if tree not in ("brkitr", "locales", "rbnf"):
            return None
        with pyio.open(os.path.join(
                os.path.dirname(__file__),
                "sample_data",
                tree,
                "LOCALE_DEPS.json"
                ), "r", encoding="utf-8-sig") as f:
            return json.load(CommentStripper(f))


class FiltrationTest(unittest.TestCase):

    def test_exclude(self):
        self._check_filter(Filter.create_from_json({
            "filterType": "exclude"
        }, TestIO()), [
        ])

    def test_default_whitelist(self):
        self._check_filter(Filter.create_from_json({
            "whitelist": [
                "ars",
                "zh_Hans"
            ]
        }, TestIO()), [
            "ars",
            "zh_Hans"
        ])

    def test_default_blacklist(self):
        expected_matches = set(EXAMPLE_FILE_STEMS)
        expected_matches.remove("ars")
        expected_matches.remove("zh_Hans")
        self._check_filter(Filter.create_from_json({
            "blacklist": [
                "ars",
                "zh_Hans"
            ]
        }, TestIO()), expected_matches)

    def test_language_whitelist(self):
        self._check_filter(Filter.create_from_json({
            "filterType": "language",
            "whitelist": [
                "af",
                "bs"
            ]
        }, TestIO()), [
            "root",
            "af_NA",
            "af_VARIANT",
            "af_ZA_VARIANT",
            "af_ZA",
            "af",
            "bs_BA",
            "bs_Cyrl_BA",
            "bs_Cyrl",
            "bs_Latn_BA",
            "bs_Latn",
            "bs"
        ])

    def test_language_blacklist(self):
        expected_matches = set(EXAMPLE_FILE_STEMS)
        expected_matches.remove("af_NA")
        expected_matches.remove("af_VARIANT")
        expected_matches.remove("af_ZA_VARIANT")
        expected_matches.remove("af_ZA")
        expected_matches.remove("af")
        self._check_filter(Filter.create_from_json({
            "filterType": "language",
            "blacklist": [
                "af"
            ]
        }, TestIO()), expected_matches)

    def test_regex_whitelist(self):
        self._check_filter(Filter.create_from_json({
            "filterType": "regex",
            "whitelist": [
                r"^ar.*$",
                r"^zh$"
            ]
        }, TestIO()), [
            "ar",
            "ar_SA",
            "ars",
            "zh"
        ])

    def test_regex_blacklist(self):
        expected_matches = set(EXAMPLE_FILE_STEMS)
        expected_matches.remove("ar")
        expected_matches.remove("ar_SA")
        expected_matches.remove("ars")
        expected_matches.remove("zh")
        self._check_filter(Filter.create_from_json({
            "filterType": "regex",
            "blacklist": [
                r"^ar.*$",
                r"^zh$"
            ]
        }, TestIO()), expected_matches)

    def test_locale_basic(self):
        self._check_filter(Filter.create_from_json({
            "filterType": "locale",
            "whitelist": [
                # Default scripts:
                # sr => Cyrl
                # vai => Vaii
                # zh => Hans
                "bs_BA", # is an alias to bs_Latn_BA
                "en_DE",
                "sr", # Language with no script
                "vai_Latn", # Language with non-default script
                "zh_Hans" # Language with default script
            ]
        }, TestIO()), [
            "root",
            # bs: should include the full dependency tree of bs_BA
            "bs_BA",
            "bs_Latn_BA",
            "bs_Latn",
            "bs",
            # en: should include the full dependency tree of en_DE
            "en",
            "en_DE",
            "en_150",
            "en_001",
            # sr: include Cyrl, the default, but not Latn.
            "sr",
            "sr_BA",
            "sr_CS",
            "sr_Cyrl",
            "sr_Cyrl_BA",
            "sr_Cyrl_CS",
            "sr_Cyrl_ME",
            # vai: include Latn but NOT Vaii.
            "vai_Latn",
            "vai_Latn_LR",
            # zh: include Hans but NOT Hant.
            "zh",
            "zh_CN",
            "zh_SG",
            "zh_Hans",
            "zh_Hans_CN",
            "zh_Hans_HK",
            "zh_Hans_MO",
            "zh_Hans_SG"
        ])

    def test_locale_no_children(self):
        self._check_filter(Filter.create_from_json({
            "filterType": "locale",
            "includeChildren": False,
            "whitelist": [
                # See comments in test_locale_basic.
                "bs_BA",
                "en_DE",
                "sr",
                "vai_Latn",
                "zh_Hans"
            ]
        }, TestIO()), [
            "root",
            "bs_BA",
            "bs_Latn_BA",
            "bs_Latn",
            "bs",
            "en",
            "en_DE",
            "en_150",
            "en_001",
            "sr",
            "vai_Latn",
            "zh",
            "zh_Hans",
        ])

    def test_locale_include_scripts(self):
        self._check_filter(Filter.create_from_json({
            "filterType": "locale",
            "includeScripts": True,
            "whitelist": [
                # See comments in test_locale_basic.
                "bs_BA",
                "en_DE",
                "sr",
                "vai_Latn",
                "zh_Hans"
            ]
        }, TestIO()), [
            "root",
            # bs: includeScripts only works for language-only (without region)
            "bs_BA",
            "bs_Latn_BA",
            "bs_Latn",
            "bs",
            # en: should include the full dependency tree of en_DE
            "en",
            "en_DE",
            "en_150",
            "en_001",
            # sr: include Latn, since no particular script was requested.
            "sr_BA",
            "sr_CS",
            "sr_Cyrl_BA",
            "sr_Cyrl_CS",
            "sr_Cyrl_ME",
            "sr_Cyrl",
            "sr_Latn_BA",
            "sr_Latn_CS",
            "sr_Latn_ME_VARIANT",
            "sr_Latn_ME",
            "sr_Latn",
            "sr_ME",
            "sr",
            # vai: do NOT include Vaii; the script was explicitly requested.
            "vai_Latn_LR",
            "vai_Latn",
            # zh: do NOT include Hant; the script was explicitly requested.
            "zh_CN",
            "zh_SG",
            "zh_Hans_CN",
            "zh_Hans_HK",
            "zh_Hans_MO",
            "zh_Hans_SG",
            "zh_Hans",
            "zh"
        ])

    def test_locale_no_children_include_scripts(self):
        self._check_filter(Filter.create_from_json({
            "filterType": "locale",
            "includeChildren": False,
            "includeScripts": True,
            "whitelist": [
                # See comments in test_locale_basic.
                "bs_BA",
                "en_DE",
                "sr",
                "vai_Latn",
                "zh_Hans"
            ]
        }, TestIO()), [
            "root",
            # bs: includeScripts only works for language-only (without region)
            "bs_BA",
            "bs_Latn_BA",
            "bs_Latn",
            "bs",
            # en: should include the full dependency tree of en_DE
            "en",
            "en_DE",
            "en_150",
            "en_001",
            # sr: include Cyrl and Latn but no other children
            "sr",
            "sr_Cyrl",
            "sr_Latn",
            # vai: include only the requested script
            "vai_Latn",
            # zh: include only the requested script
            "zh",
            "zh_Hans",
        ])

    def test_union(self):
        self._check_filter(Filter.create_from_json({
            "filterType": "union",
            "unionOf": [
                {
                    "whitelist": [
                        "ars",
                        "zh_Hans"
                    ]
                },
                {
                    "filterType": "regex",
                    "whitelist": [
                        r"^bs.*$",
                        r"^zh$"
                    ]
                }
            ]
        }, TestIO()), [
            "ars",
            "zh_Hans",
            "bs_BA",
            "bs_Cyrl_BA",
            "bs_Cyrl",
            "bs_Latn_BA",
            "bs_Latn",
            "bs",
            "zh"
        ])

    def test_hk_deps_normal(self):
        self._check_filter(Filter.create_from_json({
            "filterType": "locale",
            "whitelist": [
                "zh_HK"
            ]
        }, TestIO()), [
            "root",
            "zh_Hant",
            "zh_Hant_HK",
            "zh_HK",
        ])

    def test_hk_deps_rbnf(self):
        self._check_filter(Filter.create_from_json({
            "filterType": "locale",
            "whitelist": [
                "zh_HK"
            ]
        }, TestIO()), [
            "root",
            "yue",
            "zh_Hant_HK",
            "zh_HK",
        ], "rbnf")

    def test_no_alias_parent_structure(self):
        self._check_filter(Filter.create_from_json({
            "filterType": "locale",
            "whitelist": [
                "zh_HK"
            ]
        }, TestIO()), [
            "root",
            "zh_HK",
            "zh",
        ], "brkitr")

    def _check_filter(self, filter, expected_matches, tree="locales"):
        for file_stem in EXAMPLE_FILE_STEMS:
            is_match = filter.match(InFile("%s/%s.txt" % (tree, file_stem)))
            expected_match = file_stem in expected_matches
            self.assertEqual(is_match, expected_match, file_stem)

# Export the test for the runner
suite = unittest.makeSuite(FiltrationTest)
