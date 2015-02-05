#!/usr/bin/python

"""
JSON/YAML option parsing library and command line manipulation.

Also the entry point for running tests based on JSON options files.  See usage for more info.
"""

import json
import optparse
import os
import re
import sys

# Transparently handle YAML existing or not
try:
    import yaml
except ImportError:
    yaml = None


def json_underscore_fields(root):
    """Convert fields to underscore."""

    if isinstance(root, dict):
        for field, value in root.items():
            del root[field]
            root[field.replace("-", "_")] = json_underscore_fields(value)
    elif isinstance(root, list):
        for i in range(0, len(root)):
            root[i] = json_underscore_fields(root[i])

    return root

COMMENT_RE = \
    re.compile(
        '(^)?[^\S\n]*/(?:\*(.*?)\*/[^\S\n]*|/[^\n]*)($)?', re.DOTALL | re.MULTILINE)


def json_strip_comments(json_with_comments):
    """Strip comments from JSON strings, for easier input."""

    # Looking for comments
    match = COMMENT_RE.search(json_with_comments)
    while match:
        # single line comment
        json_with_comments = json_with_comments[
            :match.start()] + json_with_comments[match.end():]
        match = COMMENT_RE.search(json_with_comments)

    return json_with_comments


def json_update(root, new_root):
    """Recursively update a JSON document with another JSON document, merging where necessary."""

    if isinstance(root, dict) and isinstance(new_root, dict):

        for field in new_root:

            field_value = root[field] if field in root else None
            new_field_value = new_root[field]

            root[field] = json_update(field_value, new_field_value)

        return root

    return new_root


class Unset(object):

    """Special type for 'unset' JSON field, used below."""

    def __init__(self):
        pass

    def __str__(self):
        return "~"

    def __repr__(self):
        return self.__str__()


def json_update_path(root, path, value, **kwargs):
    """Update a JSON root based on a path.  Special '.'-traversal, and '*' and '**' traversal.

    Paths like "x.*.y" resolve to any path starting with x, having a single intermediate subpath,
    and ending with y.  Example: "x.a.y", "x.b.y"

    Paths like "x.**.y" resolve to any path starting with x, having zero or more intermediate
    subpaths, and ending with y.  Example: "x.y", "x.a.y", "x.b.c.y"

    """

    head_path, rest_path = split_json_path(path)

    implicit_create = kwargs[
        "implicit_create"] if "implicit_create" in kwargs else True
    push = kwargs["push"] if "push" in kwargs else False

    indent = kwargs["indent"] if "indent" in kwargs else ""
    kwargs["indent"] = indent + "  "

    # print indent, root, head_path, rest_path, kwargs

    if not head_path:

        if not push:
            return value

        else:
            # Implicitly create a root array if we need to push
            if isinstance(root, Unset):
                if not implicit_create:
                    return root
                else:
                    root = []

            if not isinstance(root, list):
                root = [root]

            root.append(value)
            return root

    # star-star-traverse all children recursively including the root itself
    if head_path == "**":

        # Don't create nonexistent child paths when star-traversing
        kwargs["implicit_create"] = False

        root_range = range(0, 0)
        if isinstance(root, dict):
            root_range = root.keys()
        elif isinstance(root, list):
            root_range = range(0, len(root))

        for field in root_range:

            # Update field children *and* field doc if ** - ** updates root
            # *and* children
            root[field] = json_update_path(
                root[field], "**." + rest_path, value, **kwargs)
            if isinstance(root[field], Unset):
                del root[field]

        # Update current root too if ** and we haven't already pushed to the
        # list
        root = json_update_path(root, rest_path, value, **kwargs)

        return root

    # don't traverse values
    if not isinstance(root, Unset) and not isinstance(root, list) and not isinstance(root, dict):
        return root

    # star-traverse docs
    if head_path == "*" and isinstance(root, dict):

        # Don't create nonexistent child paths when star-traversing
        kwargs["implicit_create"] = False

        for field in root:
            root[field] = json_update_path(
                root[field], rest_path, value, **kwargs)
            if isinstance(root[field], Unset):
                del root[field]

        return root

    # traverse lists
    if isinstance(root, list):

        root_range = None

        if head_path.isdigit():
            # numeric index arrays
            root_range = range(int(head_path), int(head_path) + 1)
        else:

            if head_path == "*":
                # Don't create nonexistent child paths when star-traversing
                kwargs["implicit_create"] = False

            # dot- or star-traverse arrays
            root_range = range(0, len(root))
            # don't consume head unless '*'
            rest_path = path if head_path != "*" else rest_path

        for i in root_range:
            root[i] = json_update_path(root[i], rest_path, value, **kwargs)
            if isinstance(root[i], Unset):
                del root[i]

        return root

    # Implicitly create a root doc if we need to keep traversing
    if isinstance(root, Unset):
        if not implicit_create:
            return root
        else:
            root = {}

    # Traverse into the dict object
    if not head_path in root:
        root[head_path] = Unset()

    root[head_path] = json_update_path(
        root[head_path], rest_path, value, **kwargs)
    if isinstance(root[head_path], Unset):
        del root[head_path]

    return root


def split_json_path(path):

    split_path = path.split(".")
    if len(split_path) == 1:
        split_path.append(".")
    rest_path = ".".join(split_path[1:])
    return (split_path[0], rest_path)


def json_coerce(json_value):
    try:
        return json.loads('[' + json_value + ']')[0]
    except:
        return json.loads('["' + json_value + '"]')[0]


def json_string_load(json_str):
    """Loads JSON data from a JSON string or a YAML string"""

    try:
        return json.loads(json_strip_comments(json_str))
    except:
        if yaml:
            return yaml.load(json_str)
        else:
            raise


def json_pipe_load(json_pipe):
    """Loads JSON data from a JSON data source or a YAML data source"""
    return json_string_load("".join(json_pipe.readlines()))


def json_file_load(json_filename):
    """Loads JSON data from a JSON file or a YAML file"""

    try:
        with open(json_filename) as json_file:
            return json_pipe_load(json_file)
    except Exception as ex:
        filebase, ext = os.path.splitext(json_filename)
        if not yaml and ext == ".yaml":
            raise Exception(("YAML library not found, cannot load %s, " +
                             "install PyYAML to correct this.") % json_filename, ex)


def json_dump(root, json_only=False):
    if json_only or not yaml:
        return json.dumps(root, sort_keys=True, indent=2)
    else:
        return yaml.safe_dump(root, default_flow_style=False)


class MultipleOption(optparse.Option):

    """Custom option class to allow parsing special JSON options by path."""

    ACTIONS = optparse.Option.ACTIONS + \
        ("extend", "json_file_update", "json_set", "json_unset", "json_push")
    STORE_ACTIONS = optparse.Option.STORE_ACTIONS + \
        ("extend", "json_file_update", "json_set", "json_unset", "json_push")
    TYPED_ACTIONS = optparse.Option.TYPED_ACTIONS + \
        ("extend", "json_file_update", "json_set", "json_unset", "json_push")
    ALWAYS_TYPED_ACTIONS = optparse.Option.ALWAYS_TYPED_ACTIONS + \
        ("extend", "json_file_update", "json_set", "json_unset", "json_push")

    def take_action(self, action, dest, opt, value, values, parser):

        if action == "extend":
            if isinstance(value, list):
                dest_values = values.ensure_value(dest, [])
                for item in value:
                    dest_values.append(item)
            else:
                values.ensure_value(dest, []).append(value)

        elif action == "json_set":

            values.ensure_value(dest, []).append(value)

            json_path, json_value = value
            if isinstance(json_value, str):
                json_value = json_coerce(json_value)

            parser.json_root = json_update_path(
                parser.json_root, json_path, json_value)

        elif action == "json_unset":

            values.ensure_value(dest, []).append(value)

            json_path = value
            parser.json_root = json_update_path(
                parser.json_root, json_path, Unset())
            if isinstance(parser.json_root, Unset):
                parser.json_root = {}

        elif action == "json_push":

            values.ensure_value(dest, []).append(value)

            json_path, json_value = value
            if isinstance(json_value, str):
                json_value = json_coerce(json_value)

            parser.json_root = json_update_path(
                parser.json_root, json_path, json_value, push=True)

        elif action == "json_file_update":

            json_filename = None
            if not value:
                # Use default value as file
                json_filename = values.ensure_value(dest, [])
            else:
                # Use specified value as file
                values.ensure_value(dest, []).append(value)
                json_filename = value

            if not os.path.isfile(json_filename):
                raise optparse.OptionValueError(
                    "cannot load json/yaml config from %s" % json_filename)

            json_data = json_file_load(json_filename)
            parser.json_root = json_update(parser.json_root, json_data)

        else:
            optparse.Option.take_action(
                self, action, dest, opt, value, values, parser)


class JSONOptionParser(optparse.OptionParser):

    """Custom option parser for JSON options.

    In addition to parsing normal options, also maintains a JSON document which can be updated by
    special --set, --unset, and --push options.

    """

    DEFAULT_USAGE = \
        """Complex JSON updates are supported via nested paths with dot separators:
    
    Ex: field-a.field-b.field-c
        
    - The --set option implicitly creates any portion of the path that does not exist, as does the \
--push option.
      
    - The --push option implicitly transforms the target of the push update into an array if not \
already an array, and adds the --push'd value to the end of the array.
      
    - The --unset option removes options by path.
    
Arrays are traversed implicitly, or you can specify an array index as a field name to traverse a \
particular array element.

JSON specified at the command line is implicitly coerced into JSON types.  To avoid ambiguity when \
specifying string arguments, you may explicitly wrap strings in double-quotes which will always \
transform into strings.

    Ex: --set tests.foo 'abcdef' -> { "tests" : { "foo" : "abcdef" } }
    Ex: --set tests.foo '{ "x" : 3 }' -> { "tests" : { "foo" : { "x" : 3 } }
    Ex: --set tests.foo '"{ \"x\" : 3 }"' -> { "tests" : { "foo" : "{ \"x\" : 3 }" }
    Ex: --set tests.foo 'true' -> { "tests" : { "foo" : true }
    Ex: --set tests.foo '"true"' -> { "tests" : { "foo" : "true" }

The special star and star-star ('*' and '**') operators allow wildcard expansion of paths.

    - '*' expands to any field at the current nesting in the path
    
    - '**' expands to *all* fields at the current or child nestings of the path - this lets one \
easily set all fields with the same names from a particular root.
      
      Ex: --set executor.**.mongod-options.nopreallocj ""

Wildcard-expanded paths are not implicitly created when they do not already exist - this also \
applies to wildcard --push operations.
    
    - The --config-file option supports loading a full YAML or JSON document from file.  Multiple \
config files can be specified, in which case the documents are merged recursively, in order of \
specification."""

    def __init__(self, add_default_options=True, configfile_args={}, *args, **kwargs):

        kwargs["option_class"] = MultipleOption
        optparse.OptionParser.__init__(self, *args, **kwargs)

        self.json_root = {}
        self.configfile_args = configfile_args

        if add_default_options:
            self.build_default_options()

    def build_default_options(self):

        help = \
            """Options specified as a JSON-formatted file, """ + \
            """applied on top of the current JSON options."""

        self.add_option('--config-file', dest='json_config_files',
                        action="json_file_update", default=[], help=help)

        help = \
            """Sets a JSON value or values along the specified path."""

        self.add_option(
            '--set', dest='json_set_values', action="json_set", nargs=2, help=help)

        help = \
            """Unsets a JSON value or values along the specified path."""

        self.add_option('--unset', dest='json_unset_values', action="json_unset", nargs=1,
                        help=help)

        help = \
            """Pushes a JSON value or values along the specified path."""

        self.add_option('--push', dest='json_unset_values', action="json_push", nargs=2,
                        help=help)

        for configfile_arg, configfile_filename in self.configfile_args.iteritems():
            self.add_option("--" + configfile_arg, dest=configfile_arg, action="json_file_update",
                            default=configfile_filename, nargs=0)

    def parse_json_args(self):
        if not sys.stdin.isatty():
            self.json_root = json_pipe_load(sys.stdin)

        values, args = self.parse_args()
        return (values, args, self.json_root)

USAGE = \
    """smoke_json.py <JSON CONFIG>

All options are specified as JSON - the json configuration can be loaded via a file and/or \
specified as options via the --set, --unset, and --push operators.

For example:
    smoke_json.py --push tests.roots "./jstests/disk/*.js" \\
                  --set suite '{}' --set executor.test-executors.jstest '{}'

results in:

    ...
    Test Configuration:
    {
      "suite": {},
      "tests": {
        "roots": [
          "./jstests/disk/*.js"
        ]
      },
      "executor": {
        "test-executors": {
          "jstest": {}
        }
      }
    }
    ...
    
""" + JSONOptionParser.DEFAULT_USAGE
