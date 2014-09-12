"""
Utilities for building a database of tests from a file system with JSON metadata files.
"""

import glob
import os
import re

from json_options import json_file_load
from json_options import json_string_load
from json_options import json_dump

JSTEST_TYPE_RE = re.compile(r"^file://.*\.js$")
DBTEST_TYPE_RE = re.compile(r"^dbtest://.*")


def guess_is_metadata_file(filename):
    filebase, ext = os.path.splitext(filename)
    return ext == ".json" or ext == ".yaml" or ext == ".yml"


def guess_test_type(uri):

    if JSTEST_TYPE_RE.match(uri):
        return "js_test"
    elif DBTEST_TYPE_RE.match(uri):
        return "db_test"
    else:
        return None


def file_uri(filepath):
    return "file://" + os.path.abspath(filepath)

FILE_URI_RE = re.compile(r"^file://(.*)")


def extract_filename(uri):
    match = FILE_URI_RE.match(uri)
    if not match:
        return None
    return match.group(1)


class Test(object):

    """A test object of a particular type, at a particular URI, with metadata.

    Often filenames are also set - though this is not required.

    """

    def __init__(self, uri=None, filename=None, test_type=None, tags=[], **metadata):

        self.uri = uri
        self.filename = os.path.abspath(filename)
        self.test_type = test_type
        self.tags = tags
        self.metadata = metadata

        if not self.uri:
            if not self.filename:
                raise Exception("Test must have either a URI or a filename specified.")
            else:
                self.uri = file_uri(self.filename)

        if not self.filename:
            self.filename = extract_filename(uri)

        if not self.test_type:
            self.test_type = guess_test_type(self.uri)

        if not self.test_type:
            raise Exception("Test at %s is of unknown type." % self.uri)

        self.rebuild_tags()

    def strip_meta_tags(self):
        ordinary_tags = []
        for tag in self.tags:
            if not tag.startswith("meta."):
                ordinary_tags.append(tag)

        return ordinary_tags

    def rebuild_tags(self):

        meta_tags = ["meta.uri.%s" % self.uri, "meta.test_type.%s" % self.test_type]
        self.tags = meta_tags + self.strip_meta_tags()

    def __str__(self):
        return "Test(%s,%s,%s)" % (self.test_type, self.uri, self.tags)

    def __repr__(self):
        return self.__str__()

    def __setstate__(self, state):
        self.__init__(**state)

    def __getstate__(self, metadata_filename=None):

        # Inline 'metadata'
        state = dict(self.__dict__.items())
        del state["metadata"]
        if len(self.metadata) > 0:
            state.update(self.metadata.items())

        # Remove "meta." tags
        state["tags"] = self.strip_meta_tags()

        # Compute relative path of filename if one exists, use instead of absolute uri
        if self.filename and metadata_filename:

            abs_filename = self.filename
            abs_metadata_path = os.path.split(os.path.abspath(metadata_filename))[0]
            common_prefix = os.path.commonprefix([abs_metadata_path, abs_filename])
            state["filename"] = os.path.relpath(abs_filename, common_prefix)
            del state["uri"]

        return state


def visit_files_matching(root,
                         file_query,
                         path_visitor,
                         file_visitor,
                         is_glob_pattern=False,
                         use_abs_paths=False):

    glob_pattern = None
    if is_glob_pattern:
        glob_pattern = root
        root = None

    if use_abs_paths:
        root = os.path.abspath(root)

    paths_seen = set([])

    def visit_file(filename):
        if file_query and not file_query.matches(filename):
            return

        parent_path, filename_only = os.path.split(filename)

        if path_visitor and not parent_path in paths_seen:
            path_visitor(parent_path)
            paths_seen.add(parent_path)

        if file_visitor:
            file_visitor(parent_path, filename_only)

    if glob_pattern:
        for filename in glob.iglob(glob_pattern):
            visit_file(filename)
    else:
        for path, dirnames, filenames in os.walk(root):
            for filename in filenames:
                visit_file(os.path.join(path, filename))

DEFAULT_TAG_FILENAME = "test_metadata.json"


def build_tests(roots, file_query=None, extract_metadata=False,
                default_metadata_filename=DEFAULT_TAG_FILENAME):
    """Builds a database (list) of tests given a number of filesystem 'roots' and a regex query.

    Searches directories recursively, and can also handle metadata files given directly as roots or
    glob-style searches.

    """

    if not roots:
        roots = ["./"]

    all_tests = {}

    def metadata_visitor(path, metadata_filename=None, test_filenames=None):

        if not metadata_filename:
            metadata_filename = default_metadata_filename

        metadata_filepath = os.path.join(path, metadata_filename)

        if not os.path.exists(metadata_filepath):
            return []

        test_metadatas = json_file_load(metadata_filepath)
        metadata_tests = {}

        if isinstance(test_metadatas, (list, tuple)):
            for test_metadata in test_metadatas:

                # The filename path is relative to the metadata file dir if not absolute
                if "filename" in test_metadata:
                    filename = test_metadata["filename"]
                    if not os.path.isabs(filename):
                        test_metadata["filename"] = os.path.join(path, filename)

                test = Test(**test_metadata)
                if test_filenames is None or test.filename in test_filenames:
                    metadata_tests[test.uri] = test
                    all_tests[test.uri] = test

        return metadata_tests.values()

    def test_visitor(path, filename):

        # The filename path is relative to the root we started the search from
        test_uri = file_uri(os.path.join(path, filename))

        if test_uri in all_tests:
            test = all_tests[test_uri]
        else:
            test = Test(filename=os.path.join(path, filename))
            all_tests[test.uri] = test

        if extract_metadata:
            extract_test_metadata(test)

    # Gather test metadata and tests

    root_metadata_files = \
        filter(lambda root: os.path.isfile(root) and guess_is_metadata_file(root), roots)
    root_test_files = \
        filter(lambda root: os.path.isfile(root) and not guess_is_metadata_file(root), roots)
    root_globs = \
        filter(lambda root: not os.path.isfile(root), roots)

    for root in root_metadata_files:
        # Load metadata from root metadata files
        metadata_tests = metadata_visitor(*os.path.split(root))
        if extract_metadata:
            # Also extract metadata from tests if we need to
            for metadata_test in metadata_tests:
                if metadata_test.filename:
                    test_visitor(*os.path.split(metadata_test.filename))

    metadata_paths = {}
    for root in root_test_files:
        metadata_path = os.path.split(root)[0]
        if metadata_path not in metadata_paths:
            metadata_paths[metadata_path] = set([])

        metadata_paths[metadata_path].add(os.path.abspath(root))

    for metadata_path, test_filenames in metadata_paths.iteritems():
        # Load metadata from test files' associated metadata files
        metadata_visitor(metadata_path, metadata_filename=None, test_filenames=test_filenames)

    for root in root_test_files:
        # Load metadata from the test itself
        test_visitor(*os.path.split(root))

    for root in root_globs:
        # If this is a directory or glob pattern, visit the directory or pattern
        # and extract metadata from metadata files and potentially test files
        is_glob_pattern = not os.path.isdir(root)
        visit_files_matching(root,
                             file_query,
                             metadata_visitor,
                             test_visitor,
                             is_glob_pattern=is_glob_pattern)

    return all_tests.values()


#
# Below is all utilities for "tags" extraction from jstests
#


JSTEST_TAGS_RE = re.compile(r".*@tags\s*:\s*(\[[^\]]*\])", re.DOTALL)


def extract_jstest_metadata(jstest):

    with open(jstest.filename) as jstest_file:
        tags_match = JSTEST_TAGS_RE.match(jstest_file.read())
        if tags_match:

            tags = None
            try:
                tags = json_string_load(tags_match.group(1))
            except Exception as ex:
                raise Exception(
                    "Could not load tags from file %s: %s" % (jstest.filename,
                                                              tags_match.group(1)), ex)
            all_tags = set(jstest.strip_meta_tags() + tags)
            jstest.tags = [tag for tag in all_tags]
            jstest.rebuild_tags()


def extract_test_metadata(test):

    if test.test_type == "js_test":
        extract_jstest_metadata(test)


def extract_metadata(tests):

    for test in tests:
        extract_test_metadata(test)


def write_metadata(tests, filename=None,
                   default_metadata_filename=DEFAULT_TAG_FILENAME,
                   json_only=False):

    metadata_file_tests = {}

    for test in tests:

        metadata_filename = filename

        if not metadata_filename:
            test_path, test_filename = os.path.split(test.filename)
            metadata_filename = os.path.join(test_path, default_metadata_filename)

        metadata_filename = os.path.abspath(metadata_filename)

        if metadata_filename not in metadata_file_tests:
            metadata_file_tests[metadata_filename] = []

        tests_in_file = metadata_file_tests[metadata_filename]
        tests_in_file.append(test)

    for metadata_filename, tests_in_file in metadata_file_tests.iteritems():
        with open(metadata_filename, 'w') as metadata_file:
            test_metadata = []
            for test in tests_in_file:
                test_metadata.append(test.__getstate__(metadata_filename))
            metadata_file.write(json_dump(test_metadata, json_only))
