#!/usr/bin/env python3
#
# Copyright (C) 2022-present MongoDB, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the Server Side Public License, version 1,
# as published by MongoDB, Inc.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# Server Side Public License for more details.
#
# You should have received a copy of the Server Side Public License
# along with this program. If not, see
# <http://www.mongodb.com/licensing/server-side-public-license>.
#
# As a special exception, the copyright holders give permission to link the
# code of portions of this program with the OpenSSL library under certain
# conditions as described in each individual source file and distribute
# linked combinations including the program with the OpenSSL library. You
# must comply with the Server Side Public License in all respects for
# all of the code used other than as permitted herein. If you modify file(s)
# with this exception, you may extend this exception to your version of the
# file(s), but you are not obligated to do so. If you do not wish to do so,
# delete this exception statement from your version. If you delete this
# exception statement from all source files in the program, then also delete
# it in the license file.
"""Generate source files from a specification of builtin roles."""

import argparse
import sys

import yaml
from Cheetah.Template import Template

help_epilog = """
The builtin_roles_spec YAML document is a mapping containing one toplevel field:

    `roles` is a mapping for role name to role definitions.

    Each role definition may have 3 possible keys:
        `roles`: (list) A set of references back to other roles
             which this role should inherit from.
             The list item may either be a string to indicate
             a role on the same database,
             or a tuple of {role: 'roleName', db: 'targetDb'}.
    `adminOnly`: (bool) True if this role exist on the
             `admin` database only. False (default) otherwise.
    `privileges`: (list) A set of ResourcePattern and PrivilegeVector tuples.
             Each element mat have the following fields:
                `matchType`: (string, required)
                            One of the MatchType enum values
                            defined in mongo/db/auth/action_types.yml
                `db`: (string) If `matchType` expects a DB (e.g. 'database', 'exact_namespace'),
                            then `db` is the database that the role is defined on by default,
                            but it may be overridden to reference privileges in other DBs.
                            Note that overriding DB is only valid for roles on the admin DB.
                `collection`: (string) If `matchType` expects a collection
                            (e.g. 'collection', 'exact_namespace'), then `collection`
                            is required and provides the obvious meaning.
                `actions`: (list, required) A set of ActionTypes granted for this ResourcePattern.
                            The elements are nominally strings matching those from action_types.yml,
                            however to make compositing lists easier, elements may be lists of strings
                            and will be flattened during the generation process.
                `tenancy`: (one of 'any', 'single', 'multi', 'system', or 'tenant', default: any)
                            'any': This privilege always applies to the role.
                            'single': Only applies when in single-tenancy mode.
                            'multi': Only applies when in multi-tenancy mode.
                            'system': Only applies in single tenancy or to the "system" tenant in multi-tenancy
                            'tenant': Only applies to non-system tenants in multi-tenancy.
"""


def init_parser():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=__doc__,
        epilog=help_epilog,
    )
    parser.add_argument("--verbose", action="store_true", help="extra debug logging to stderr")
    parser.add_argument("builtin_roles_spec", help="YAML file describing builtin roles")
    parser.add_argument("template_file", help="Cheetah template file")
    parser.add_argument("output_file")
    return parser


def render_template(template_path, **kw):
    """Renders the template file located at template_path, using the variables defined by kw, and
    returns the result as a string"""

    template = Template.compile(
        file=template_path,
        compilerSettings=dict(
            directiveStartToken="//#", directiveEndToken="//#", commentStartToken="//##"
        ),
        baseclass=dict,
        useCache=False,
    )
    return str(template(**kw))


def check_allowed_fields(mapping, allowed):
    for field in mapping:
        if field not in allowed:
            raise Exception("Unknown field '%s' in %r" % (field, mapping))


def check_required_fields(mapping, required):
    for field in required:
        if field not in mapping:
            raise Exception("Missing required field '%s' in %r" % (field, mapping))


def assert_str_field(name, value):
    if type(value) is not str:
        raise Exception(
            "Invalid type for string field '%s', got '%s' ': %r" % (name, type(value), value)
        )


def assert_list_field(name, value):
    if type(value) is not list:
        raise Exception(
            "Invalid type for list field '%s', got '%s' ': %r" % (name, type(value), value)
        )


def get_nonempty_str_field(mapping, fieldName):
    assert_str_field(fieldName, mapping[fieldName])
    if len(mapping[fieldName]) == 0:
        raise Exception("Field '%s' value must be a non-empty string" % (fieldName))
    return mapping[fieldName]


class InheritedRole:
    def __init__(self, spec):
        self.db = None

        if type(spec) is str:
            self.role = spec
        else:
            if type(spec) is not dict:
                raise Exception(
                    "Inherited role must be either a simple name, or a role/db tuple, got: %r"
                    % (spec)
                )
            check_allowed_fields(spec, ["role", "db"])
            check_required_fields(spec, ["role"])
            self.role = get_nonempty_str_field(spec, "role")
            if "db" in spec:
                self.db = get_nonempty_str_field(spec, "db")


class Privilege:
    def __init__(self, spec):
        check_allowed_fields(
            spec, ["matchType", "db", "collection", "system_buckets", "actions", "tenancy"]
        )
        check_required_fields(spec, ["matchType", "actions"])

        self.matchType = get_nonempty_str_field(spec, "matchType")
        self.db = None
        self.collection = None
        self.system_buckets = None
        self.actions = []
        self.tenancy = "any"

        db_valid_types = [
            "database",
            "exact_namespace",
            "system_buckets",
            "any_system_buckets_in_db",
        ]
        if "db" in spec:
            if self.matchType in db_valid_types:
                self.db = get_nonempty_str_field(spec, "db")
            else:
                raise Exception("db field is not valid for matchType: %s" % (self.matchType))

        coll_valid_types = ["collection", "exact_namespace"]
        if self.matchType in coll_valid_types:
            check_required_fields(spec, ["collection"])
            self.collection = get_nonempty_str_field(spec, "collection")
        elif "collection" in spec:
            raise Exception("collection field is not valid for matchType: %s" % (self.matchType))

        buckets_valid_types = ["system_buckets", "system_buckets_in_any_db"]
        if self.matchType in buckets_valid_types:
            check_required_fields(spec, ["system_buckets"])
            db.system_buckets = get_nonempty_str_field(spec, "system_buckets")
        elif "system_buckets" in spec:
            raise Exception(
                "system_buckets field is not valid for matchType: %s" % (self.matchType)
            )

        assert_list_field("actions", spec["actions"])
        for action in spec["actions"]:
            if type(action) is list:
                for subaction in action:
                    assert_str_field("actions", subaction)
                    self.actions.append(subaction)
            else:
                assert_str_field("actions", action)
                self.actions.append(action)

        if "tenancy" in spec:
            assert_str_field("tenancy", spec["tenancy"])
            tenancy_options = ["any", "single", "multi", "system", "tenant"]
            if spec["tenancy"] not in tenancy_options:
                raise Exception(
                    "Invalid value for enum field 'tenancy', got '%s', expeted one of %r"
                    % (spec["tenancy"], tenancy_options)
                )
            self.tenancy = spec["tenancy"]


class BuiltinRole:
    def __init__(self, name, spec):
        self.name = name
        self.adminOnly = False
        self.roles = []
        self.privileges = []

        check_allowed_fields(spec, ["adminOnly", "roles", "privileges"])

        if "adminOnly" in spec:
            if type(spec["adminOnly"]) is not bool:
                raise Exception("adminOnly must be a bool, got: %r" % (spec["adminOnly"]))
            self.adminOnly = spec["adminOnly"]

        if "roles" in spec:
            assert_list_field("roles", spec["roles"])
            for role in spec["roles"]:
                self.roles.append(InheritedRole(role))

        if "privileges" in spec:
            assert_list_field("privileges", spec["privileges"])
            for priv in spec["privileges"]:
                self.privileges.append(Privilege(priv))


def parse_builtin_role_definitions_from_file(roles_filename, verbose=False):
    roles = []
    with open(roles_filename, "r") as roles_file:
        doc = yaml.safe_load(roles_file)

    if verbose:
        yaml.dump(doc, sys.stderr)

    for roleName in doc["roles"]:
        roles.append(BuiltinRole(roleName, doc["roles"][roleName]))

    return roles


def main():
    parsed = init_parser().parse_args()
    verbose = parsed.verbose
    template_file = parsed.template_file
    output_file = parsed.output_file

    # Parse and validate builtin_roles.yml
    builtin_roles = parse_builtin_role_definitions_from_file(parsed.builtin_roles_spec, verbose)

    # Render the templates to the output files.
    if verbose:
        print(f"rendering {template_file} => {output_file}")
    text = render_template(template_file, roles=builtin_roles)
    with open(output_file, "w") as outfile:
        outfile.write(text)


if __name__ == "__main__":
    main()
