# Copyright (C) 2018 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html

# Python 2/3 Compatibility (ICU-20299)
# TODO(ICU-20301): Remove this.
from __future__ import print_function

from abc import abstractmethod
from collections import defaultdict
import re
import sys

from . import *
from . import utils
from .request_types import *


# Note: for this to be a proper abstract class, it should extend abc.ABC.
# There is no nice way to do this that works in both Python 2 and 3.
# TODO(ICU-20301): Make this inherit from abc.ABC.
class Filter(object):
    @staticmethod
    def create_from_json(json_data, io):
        assert io != None
        if "filterType" in json_data:
            filter_type = json_data["filterType"]
        else:
            filter_type = "file-stem"

        if filter_type == "file-stem":
            return FileStemFilter(json_data)
        elif filter_type == "language":
            return LanguageFilter(json_data)
        elif filter_type == "regex":
            return RegexFilter(json_data)
        elif filter_type == "exclude":
            return ExclusionFilter()
        elif filter_type == "union":
            return UnionFilter(json_data, io)
        elif filter_type == "locale":
            return LocaleFilter(json_data, io)
        else:
            print("Error: Unknown filterType option: %s" % filter_type, file=sys.stderr)
            return None

    def filter(self, request):
        if not request.apply_file_filter(self):
            return []
        for file in request.all_input_files():
            assert self.match(file)
        return [request]

    @staticmethod
    def _file_to_file_stem(file):
        start = file.filename.rfind("/")
        limit = file.filename.rfind(".")
        return file.filename[start+1:limit]

    @staticmethod
    def _file_to_subdir(file):
        limit = file.filename.rfind("/")
        if limit == -1:
            return None
        return file.filename[:limit]

    @abstractmethod
    def match(self, file):
        pass


class InclusionFilter(Filter):
    def match(self, file):
        return True


class ExclusionFilter(Filter):
    def match(self, file):
        return False


class IncludeExcludeFilter(Filter):
    def __init__(self, json_data):
        if "whitelist" in json_data:
            self.is_includelist = True
            self.includelist = json_data["whitelist"]
        elif "includelist" in json_data:
            self.is_includelist = True
            self.includelist = json_data["includelist"]
        elif "blacklist" in json_data:
            self.is_includelist = False
            self.excludelist = json_data["blacklist"]
        elif "excludelist" in json_data:
            self.is_includelist = False
            self.excludelist = json_data["excludelist"]
        else:
            raise AssertionError("Need either includelist or excludelist: %s" % str(json_data))

    def match(self, file):
        file_stem = self._file_to_file_stem(file)
        return self._should_include(file_stem)

    @abstractmethod
    def _should_include(self, file_stem):
        pass


class FileStemFilter(IncludeExcludeFilter):
    def _should_include(self, file_stem):
        if self.is_includelist:
            return file_stem in self.includelist
        else:
            return file_stem not in self.excludelist


class LanguageFilter(IncludeExcludeFilter):
    def _should_include(self, file_stem):
        language = file_stem.split("_")[0]
        if language == "root":
            # Always include root.txt
            return True
        if self.is_includelist:
            return language in self.includelist
        else:
            return language not in self.excludelist


class RegexFilter(IncludeExcludeFilter):
    def __init__(self, *args):
        # TODO(ICU-20301): Change this to: super().__init__(*args)
        super(RegexFilter, self).__init__(*args)
        if self.is_includelist:
            self.includelist = [re.compile(pat) for pat in self.includelist]
        else:
            self.excludelist = [re.compile(pat) for pat in self.excludelist]

    def _should_include(self, file_stem):
        if self.is_includelist:
            for pattern in self.includelist:
                if pattern.match(file_stem):
                    return True
            return False
        else:
            for pattern in self.excludelist:
                if pattern.match(file_stem):
                    return False
            return True


class UnionFilter(Filter):
    def __init__(self, json_data, io):
        # Collect the sub-filters.
        self.sub_filters = []
        for filter_json in json_data["unionOf"]:
            self.sub_filters.append(Filter.create_from_json(filter_json, io))

    def match(self, file):
        """Match iff any of the sub-filters match."""
        for filter in self.sub_filters:
            if filter.match(file):
                return True
        return False


LANGUAGE_SCRIPT_REGEX = re.compile(r"^([a-z]{2,3})_[A-Z][a-z]{3}$")
LANGUAGE_ONLY_REGEX = re.compile(r"^[a-z]{2,3}$")

class LocaleFilter(Filter):
    def __init__(self, json_data, io):
        if "whitelist" in json_data:
            self.locales_requested = list(json_data["whitelist"])
        elif "includelist" in json_data:
            self.locales_requested = list(json_data["includelist"])
        else:
            raise AssertionError("You must have an includelist in a locale filter")
        self.include_children = json_data.get("includeChildren", True)
        self.include_scripts = json_data.get("includeScripts", False)

        # Load the dependency graph from disk
        self.dependency_data_by_tree = {
            tree: io.read_locale_deps(tree)
            for tree in utils.ALL_TREES
        }

    def match(self, file):
        tree = self._file_to_subdir(file)
        assert tree is not None
        locale = self._file_to_file_stem(file)

        # A locale is *required* if it is *requested* or an ancestor of a
        # *requested* locale.
        if locale in self._locales_required(tree):
            return True

        # Resolve include_scripts and include_children.
        return self._match_recursive(locale, tree)

    def _match_recursive(self, locale, tree):
        # Base case: return True if we reached a *requested* locale,
        # or False if we ascend out of the locale tree.
        if locale is None:
            return False
        if locale in self.locales_requested:
            return True

        # Check for alternative scripts.
        # This causes sr_Latn to check sr instead of going directly to root.
        if self.include_scripts:
            match = LANGUAGE_SCRIPT_REGEX.match(locale)
            if match and self._match_recursive(match.group(1), tree):
                return True

        # Check if we are a descendant of a *requested* locale.
        if self.include_children:
            parent = self._get_parent_locale(locale, tree)
            if self._match_recursive(parent, tree):
                return True

        # No matches.
        return False

    def _get_parent_locale(self, locale, tree):
        """Gets the parent locale in the given tree, according to dependency data."""
        dependency_data = self.dependency_data_by_tree[tree]
        if "parents" in dependency_data and locale in dependency_data["parents"]:
            return dependency_data["parents"][locale]
        if "aliases" in dependency_data and locale in dependency_data["aliases"]:
            return dependency_data["aliases"][locale]
        if LANGUAGE_ONLY_REGEX.match(locale):
            return "root"
        i = locale.rfind("_")
        if i < 0:
            assert locale == "root", "Invalid locale: %s/%s" % (tree, locale)
            return None
        return locale[:i]

    def _locales_required(self, tree):
        """Returns a generator of all required locales in the given tree."""
        for locale in self.locales_requested:
            while locale is not None:
                yield locale
                locale = self._get_parent_locale(locale, tree)


def apply_filters(requests, config, io):
    """Runs the filters and returns a new list of requests."""
    requests = _apply_file_filters(requests, config, io)
    requests = _apply_resource_filters(requests, config, io)
    return requests


def _apply_file_filters(old_requests, config, io):
    """Filters out entire files."""
    filters = _preprocess_file_filters(old_requests, config, io)
    new_requests = []
    for request in old_requests:
        category = request.category
        if category in filters:
            new_requests += filters[category].filter(request)
        else:
            new_requests.append(request)
    return new_requests


def _preprocess_file_filters(requests, config, io):
    all_categories = set(
        request.category
        for request in requests
    )
    all_categories.remove(None)
    all_categories = list(sorted(all_categories))
    json_data = config.filters_json_data
    filters = {}
    default_filter_json = "exclude" if config.strategy == "additive" else "include"
    for category in all_categories:
        filter_json = default_filter_json
        # Figure out the correct filter to create
        if "featureFilters" in json_data and category in json_data["featureFilters"]:
            filter_json = json_data["featureFilters"][category]
        if filter_json == "include" and "localeFilter" in json_data and category.endswith("_tree"):
            filter_json = json_data["localeFilter"]
        # Resolve the filter JSON into a filter object
        if filter_json == "exclude":
            filters[category] = ExclusionFilter()
        elif filter_json == "include":
            pass  # no-op
        else:
            filters[category] = Filter.create_from_json(filter_json, io)
    if "featureFilters" in json_data:
        for category in json_data["featureFilters"]:
            if category not in all_categories:
                print("Warning: category %s is not known" % category, file=sys.stderr)
    return filters


class ResourceFilterInfo(object):
    def __init__(self, category, strategy):
        self.category = category
        self.strategy = strategy
        self.filter_tmp_dir = "filters/%s" % category
        self.input_files = None
        self.filter_files = None
        self.rules_by_file = None

    def apply_to_requests(self, all_requests):
        # Call this method only once per list of requests.
        assert self.input_files is None
        for request in all_requests:
            if request.category != self.category:
                continue
            if not isinstance(request, AbstractExecutionRequest):
                continue
            if request.tool != IcuTool("genrb"):
                continue
            if not request.input_files:
                continue
            self._set_files(request.input_files)
            request.dep_targets += [self.filter_files[:]]
            arg_str = "--filterDir {TMP_DIR}/%s" % self.filter_tmp_dir
            request.args = "%s %s" % (arg_str, request.args)

        # Make sure we found the target request
        if self.input_files is None:
            print("WARNING: Category not found: %s" % self.category, file=sys.stderr)
            self.input_files = []
            self.filter_files = []
            self.rules_by_file = []

    def _set_files(self, files):
        # Note: The input files to genrb for a certain category should always
        # be the same. For example, there are often two genrb calls: one for
        # --writePoolBundle, and the other for --usePoolBundle. They are both
        # expected to have the same list of input files.
        if self.input_files is not None:
            assert self.input_files == files
            return
        self.input_files = list(files)
        self.filter_files = [
            TmpFile("%s/%s" % (self.filter_tmp_dir, basename))
            for basename in (
                file.filename[file.filename.rfind("/")+1:]
                for file in files
            )
        ]
        if self.strategy == "additive":
            self.rules_by_file = [
                [r"-/", r"+/%%ALIAS", r"+/%%Parent"]
                for _ in range(len(files))
            ]
        else:
            self.rules_by_file = [
                [r"+/"]
                for _ in range(len(files))
            ]

    def add_rules(self, file_filter, rules):
        for file, rule_list in zip(self.input_files, self.rules_by_file):
            if file_filter.match(file):
                rule_list += rules

    def make_requests(self):
        # Map from rule list to filter files with that rule list
        unique_rules = defaultdict(list)
        for filter_file, rules in zip(self.filter_files, self.rules_by_file):
            unique_rules[tuple(rules)].append(filter_file)

        new_requests = []
        i = 0
        for rules, filter_files in unique_rules.items():
            base_filter_file = filter_files[0]
            new_requests += [
                PrintFileRequest(
                    name = "%s_print_%d" % (self.category, i),
                    output_file = base_filter_file,
                    content = self._generate_resource_filter_txt(rules)
                )
            ]
            i += 1
            for filter_file in filter_files[1:]:
                new_requests += [
                    CopyRequest(
                        name = "%s_copy_%d" % (self.category, i),
                        input_file = base_filter_file,
                        output_file = filter_file
                    )
                ]
                i += 1
        return new_requests

    @staticmethod
    def _generate_resource_filter_txt(rules):
        result = "# Caution: This file is automatically generated\n\n"
        result += "\n".join(rules)
        return result


def _apply_resource_filters(all_requests, config, io):
    """Creates filters for looking within resource bundle files."""
    json_data = config.filters_json_data
    if "resourceFilters" not in json_data:
        return all_requests

    collected = {}
    for entry in json_data["resourceFilters"]:
        if "files" in entry:
            file_filter = Filter.create_from_json(entry["files"], io)
        else:
            file_filter = InclusionFilter()
        for category in entry["categories"]:
            # not defaultdict because we need to pass arguments to the constructor
            if category not in collected:
                filter_info = ResourceFilterInfo(category, config.strategy)
                filter_info.apply_to_requests(all_requests)
                collected[category] = filter_info
            else:
                filter_info = collected[category]
            filter_info.add_rules(file_filter, entry["rules"])

    # Add the filter generation requests to the beginning so that by default
    # they are made before genrb gets run (order is required by windirect)
    new_requests = []
    for filter_info in collected.values():
        new_requests += filter_info.make_requests()
    new_requests += all_requests
    return new_requests
