# Copyright (C) 2018 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html

# Python 2/3 Compatibility (ICU-20299)
# TODO(ICU-20301): Remove this.
from __future__ import print_function

from abc import abstractmethod
import copy
import sys

from . import *
from . import utils


# TODO(ICU-20301): Remove arguments from all instances of super() in this file

# Note: for this to be a proper abstract class, it should extend abc.ABC.
# There is no nice way to do this that works in both Python 2 and 3.
# TODO(ICU-20301): Make this inherit from abc.ABC.
class AbstractRequest(object):
    def __init__(self, **kwargs):

        # Used for identification purposes
        self.name = None

        # The filter category that applies to this request
        self.category = None

        self._set_fields(kwargs)

    def _set_fields(self, kwargs):
        for key, value in list(kwargs.items()):
            if hasattr(self, key):
                if isinstance(value, list):
                    value = copy.copy(value)
                elif isinstance(value, dict):
                    value = copy.deepcopy(value)
                setattr(self, key, value)
            else:
                raise ValueError("Unknown argument: %s" % key)

    def apply_file_filter(self, filter):
        """
        Returns True if this request still has input files after filtering,
        or False if the request is "empty" after filtering.
        """
        return True

    def flatten(self, config, all_requests, common_vars):
        return [self]

    def all_input_files(self):
        return []

    def all_output_files(self):
        return []


class AbstractExecutionRequest(AbstractRequest):
    def __init__(self, **kwargs):

        # Names of targets (requests) or files that this request depends on.
        # The entries of dep_targets may be any of the following types:
        #
        #   1. DepTarget, for the output of an execution request.
        #   2. InFile, TmpFile, etc., for a specific file.
        #   3. A list of InFile, TmpFile, etc., where the list is the same
        #      length as self.input_files and self.output_files.
        #
        # In cases 1 and 2, the dependency is added to all rules that the
        # request generates. In case 3, the dependency is added only to the
        # rule that generates the output file at the same array index.
        self.dep_targets = []

        # Computed during self.flatten(); don't edit directly.
        self.common_dep_files = []

        # Primary input files
        self.input_files = []

        # Output files; for some subclasses, this must be the same length
        # as input_files
        self.output_files = []

        # What tool to execute
        self.tool = None

        # Argument string to pass to the tool with optional placeholders
        self.args = ""

        # Placeholders to substitute into the argument string; if any of these
        # have a list type, the list must be equal in length to input_files
        self.format_with = {}

        super(AbstractExecutionRequest, self).__init__(**kwargs)

    def apply_file_filter(self, filter):
        i = 0
        while i < len(self.input_files):
            if filter.match(self.input_files[i]):
                i += 1
            else:
                self._del_at(i)
        return i > 0

    def _del_at(self, i):
        del self.input_files[i]
        for _, v in self.format_with.items():
            if isinstance(v, list):
                assert len(v) == len(self.input_files) + 1
                del v[i]
        for v in self.dep_targets:
            if isinstance(v, list):
                assert len(v) == len(self.input_files) + 1
                del v[i]

    def flatten(self, config, all_requests, common_vars):
        self._dep_targets_to_files(all_requests)
        return super(AbstractExecutionRequest, self).flatten(config, all_requests, common_vars)

    def _dep_targets_to_files(self, all_requests):
        if not self.dep_targets:
            return
        for dep_target in self.dep_targets:
            if isinstance(dep_target, list):
                if hasattr(self, "specific_dep_files"):
                    assert len(dep_target) == len(self.specific_dep_files)
                    for file, out_list in zip(dep_target, self.specific_dep_files):
                        assert hasattr(file, "filename")
                        out_list.append(file)
                else:
                    self.common_dep_files += dep_target
                continue
            if not isinstance(dep_target, DepTarget):
                # Copy file entries directly to dep_files.
                assert hasattr(dep_target, "filename")
                self.common_dep_files.append(dep_target)
                continue
            # For DepTarget entries, search for the target.
            for request in all_requests:
                if request.name == dep_target.name:
                    self.common_dep_files += request.all_output_files()
                    break
            else:
                print("Warning: Unable to find target %s, a dependency of %s" % (
                    dep_target.name,
                    self.name
                ), file=sys.stderr)
        self.dep_targets = []

    def all_input_files(self):
        return self.common_dep_files + self.input_files

    def all_output_files(self):
        return self.output_files


class SingleExecutionRequest(AbstractExecutionRequest):
    def __init__(self, **kwargs):
        super(SingleExecutionRequest, self).__init__(**kwargs)


class RepeatedExecutionRequest(AbstractExecutionRequest):
    def __init__(self, **kwargs):

        # Placeholders to substitute into the argument string unique to each
        # iteration; all values must be lists equal in length to input_files
        self.repeat_with = {}

        # Lists for dep files that are specific to individual resource bundle files
        self.specific_dep_files = [[] for _ in range(len(kwargs["input_files"]))]

        super(RepeatedExecutionRequest, self).__init__(**kwargs)

    def _del_at(self, i):
        super(RepeatedExecutionRequest, self)._del_at(i)
        del self.output_files[i]
        del self.specific_dep_files[i]
        for _, v in self.repeat_with.items():
            if isinstance(v, list):
                del v[i]

    def all_input_files(self):
        files = super(RepeatedExecutionRequest, self).all_input_files()
        for specific_file_list in self.specific_dep_files:
            files += specific_file_list
        return files


class RepeatedOrSingleExecutionRequest(AbstractExecutionRequest):
    def __init__(self, **kwargs):
        self.repeat_with = {}
        super(RepeatedOrSingleExecutionRequest, self).__init__(**kwargs)

    def flatten(self, config, all_requests, common_vars):
        if config.max_parallel:
            new_request = RepeatedExecutionRequest(
                name = self.name,
                category = self.category,
                dep_targets = self.dep_targets,
                input_files = self.input_files,
                output_files = self.output_files,
                tool = self.tool,
                args = self.args,
                format_with = self.format_with,
                repeat_with = self.repeat_with
            )
        else:
            new_request = SingleExecutionRequest(
                name = self.name,
                category = self.category,
                dep_targets = self.dep_targets,
                input_files = self.input_files,
                output_files = self.output_files,
                tool = self.tool,
                args = self.args,
                format_with = utils.concat_dicts(self.format_with, self.repeat_with)
            )
        return new_request.flatten(config, all_requests, common_vars)

    def _del_at(self, i):
        super(RepeatedOrSingleExecutionRequest, self)._del_at(i)
        del self.output_files[i]
        for _, v in self.repeat_with.items():
            if isinstance(v, list):
                del v[i]


class PrintFileRequest(AbstractRequest):
    def __init__(self, **kwargs):
        self.output_file = None
        self.content = None
        super(PrintFileRequest, self).__init__(**kwargs)

    def all_output_files(self):
        return [self.output_file]


class CopyRequest(AbstractRequest):
    def __init__(self, **kwargs):
        self.input_file = None
        self.output_file = None
        super(CopyRequest, self).__init__(**kwargs)

    def all_input_files(self):
        return [self.input_file]

    def all_output_files(self):
        return [self.output_file]


class VariableRequest(AbstractRequest):
    def __init__(self, **kwargs):
        self.input_files = []
        super(VariableRequest, self).__init__(**kwargs)

    def all_input_files(self):
        return self.input_files


class ListRequest(AbstractRequest):
    def __init__(self, **kwargs):
        self.variable_name = None
        self.output_file = None
        self.include_tmp = None
        super(ListRequest, self).__init__(**kwargs)

    def flatten(self, config, all_requests, common_vars):
        list_files = list(sorted(utils.get_all_output_files(all_requests)))
        if self.include_tmp:
            variable_files = list(sorted(utils.get_all_output_files(all_requests, include_tmp=True)))
        else:
            # Always include the list file itself
            variable_files = list_files + [self.output_file]
        return PrintFileRequest(
            name = self.name,
            output_file = self.output_file,
            content = "\n".join(file.filename for file in list_files)
        ).flatten(config, all_requests, common_vars) + VariableRequest(
            name = self.variable_name,
            input_files = variable_files
        ).flatten(config, all_requests, common_vars)

    def all_output_files(self):
        return [self.output_file]


class IndexRequest(AbstractRequest):
    def __init__(self, **kwargs):
        self.installed_files = []
        self.alias_files = []
        self.txt_file = None
        self.output_file = None
        self.cldr_version = ""
        self.args = ""
        self.format_with = {}
        super(IndexRequest, self).__init__(**kwargs)

    def apply_file_filter(self, filter):
        i = 0
        while i < len(self.installed_files):
            if filter.match(self.installed_files[i]):
                i += 1
            else:
                del self.installed_files[i]
        j = 0
        while j < len(self.alias_files):
            if filter.match(self.alias_files[j]):
                j += 1
            else:
                del self.alias_files[j]
        return i + j > 0

    def flatten(self, config, all_requests, common_vars):
        return (
            PrintFileRequest(
                name = self.name,
                output_file = self.txt_file,
                content = self._generate_index_file(common_vars)
            ).flatten(config, all_requests, common_vars) +
            SingleExecutionRequest(
                name = "%s_res" % self.name,
                category = self.category,
                input_files = [self.txt_file],
                output_files = [self.output_file],
                tool = IcuTool("genrb"),
                args = self.args,
                format_with = self.format_with
            ).flatten(config, all_requests, common_vars)
        )

    def _generate_index_file(self, common_vars):
        installed_locales = [IndexRequest.locale_file_stem(f) for f in self.installed_files]
        alias_locales = [IndexRequest.locale_file_stem(f) for f in self.alias_files]
        formatted_version = "    CLDRVersion { \"%s\" }\n" % self.cldr_version if self.cldr_version else ""
        formatted_installed_locales = "\n".join(["        %s {\"\"}" % v for v in installed_locales])
        formatted_alias_locales = "\n".join(["        %s {\"\"}" % v for v in alias_locales])
        # TODO: CLDRVersion is required only in the base file
        return ("// Warning this file is automatically generated\n"
                "{INDEX_NAME}:table(nofallback) {{\n"
                "{FORMATTED_VERSION}"
                "    InstalledLocales:table {{\n"
                "{FORMATTED_INSTALLED_LOCALES}\n"
                "    }}\n"
                "    AliasLocales:table {{\n"
                "{FORMATTED_ALIAS_LOCALES}\n"
                "    }}\n"
                "}}").format(
                    FORMATTED_VERSION = formatted_version,
                    FORMATTED_INSTALLED_LOCALES = formatted_installed_locales,
                    FORMATTED_ALIAS_LOCALES = formatted_alias_locales,
                    **common_vars
                )

    def all_input_files(self):
        return self.installed_files + self.alias_files

    def all_output_files(self):
        return [self.output_file]

    @staticmethod
    def locale_file_stem(f):
        return f.filename[f.filename.rfind("/")+1:-4]
