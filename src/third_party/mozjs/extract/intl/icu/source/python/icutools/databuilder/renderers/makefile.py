# Copyright (C) 2018 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html

# Python 2/3 Compatibility (ICU-20299)
# TODO(ICU-20301): Remove this.
from __future__ import print_function

from . import *
from .. import *
from .. import utils
from ..request_types import *

def get_gnumake_rules(build_dirs, requests, makefile_vars, **kwargs):
    makefile_string = ""

    # Common Variables
    common_vars = kwargs["common_vars"]
    for key, value in sorted(makefile_vars.items()):
        makefile_string += "{KEY} = {VALUE}\n".format(
            KEY = key,
            VALUE = value
        )
    makefile_string += "\n"

    # Directories
    dirs_timestamp_file = "{TMP_DIR}/dirs.timestamp".format(**common_vars)
    makefile_string += "DIRS = {TIMESTAMP_FILE}\n\n".format(
        TIMESTAMP_FILE = dirs_timestamp_file
    )
    makefile_string += "{TIMESTAMP_FILE}:\n\t$(MKINSTALLDIRS) {ALL_DIRS}\n\techo timestamp > {TIMESTAMP_FILE}\n\n".format(
        TIMESTAMP_FILE = dirs_timestamp_file,
        ALL_DIRS = " ".join(build_dirs).format(**common_vars)
    )

    # Generate Rules
    make_rules = []
    for request in requests:
        make_rules += get_gnumake_rules_helper(request, **kwargs)

    # Main Commands
    for rule in make_rules:
        if isinstance(rule, MakeFilesVar):
            makefile_string += "{NAME} = {FILE_LIST}\n\n".format(
                NAME = rule.name,
                FILE_LIST = files_to_makefile(rule.files, wrap = True, **kwargs),
            )
            continue

        if isinstance(rule, MakeStringVar):
            makefile_string += "define {NAME}\n{CONTENT}\nendef\nexport {NAME}\n\n".format(
                NAME = rule.name,
                CONTENT = rule.content
            )
            continue

        assert isinstance(rule, MakeRule)
        header_line = "{OUT_FILE}: {DEP_FILES} {DEP_LITERALS} | $(DIRS)".format(
            OUT_FILE = files_to_makefile([rule.output_file], **kwargs),
            DEP_FILES = files_to_makefile(rule.dep_files, wrap = True, **kwargs),
            DEP_LITERALS = " ".join(rule.dep_literals)
        )

        if len(rule.cmds) == 0:
            makefile_string += "%s\n\n" % header_line
            continue

        makefile_string += "{HEADER_LINE}\n{RULE_LINES}\n\n".format(
            HEADER_LINE = header_line,
            RULE_LINES = "\n".join("\t%s" % cmd for cmd in rule.cmds)
        )

    return makefile_string

def files_to_makefile(files, common_vars, wrap = False, **kwargs):
    if len(files) == 0:
        return ""
    dirnames = [utils.dir_for(file).format(**common_vars) for file in files]
    join_str = " \\\n\t\t" if wrap and len(files) > 2 else " "
    if len(files) == 1:
        return "%s/%s" % (dirnames[0], files[0].filename)
    elif len(set(dirnames)) == 1:
        return "$(addprefix %s/,%s)" % (dirnames[0], join_str.join(file.filename for file in files))
    else:
        return join_str.join("%s/%s" % (d, f.filename) for d,f in zip(dirnames, files))

def get_gnumake_rules_helper(request, common_vars, **kwargs):

    if isinstance(request, PrintFileRequest):
        var_name = "%s_CONTENT" % request.name.upper()
        return [
            MakeStringVar(
                name = var_name,
                content = request.content
            ),
            MakeRule(
                name = request.name,
                dep_literals = [],
                dep_files = [],
                output_file = request.output_file,
                cmds = [
                    "echo \"$${VAR_NAME}\" > {MAKEFILENAME}".format(
                        VAR_NAME = var_name,
                        MAKEFILENAME = files_to_makefile([request.output_file], common_vars),
                        **common_vars
                    )
                ]
            )
        ]


    if isinstance(request, CopyRequest):
        return [
            MakeRule(
                name = request.name,
                dep_literals = [],
                dep_files = [request.input_file],
                output_file = request.output_file,
                cmds = ["cp %s %s" % (
                    files_to_makefile([request.input_file], common_vars),
                    files_to_makefile([request.output_file], common_vars))
                ]
            )
        ]

    if isinstance(request, VariableRequest):
        return [
            MakeFilesVar(
                name = request.name.upper(),
                files = request.input_files
            )
        ]

    if request.tool.name == "make":
        cmd_template = "$(MAKE) {ARGS}"
    elif request.tool.name == "gentest":
        cmd_template = "$(INVOKE) $(GENTEST) {ARGS}"
    else:
        assert isinstance(request.tool, IcuTool)
        cmd_template = "$(INVOKE) $(TOOLBINDIR)/{TOOL} {{ARGS}}".format(
            TOOL = request.tool.name
        )

    if isinstance(request, SingleExecutionRequest):
        cmd = utils.format_single_request_command(request, cmd_template, common_vars)
        dep_files = request.all_input_files()

        if len(request.output_files) > 1:
            # Special case for multiple output files: Makefile rules should have only one
            # output file apiece. More information:
            # https://www.gnu.org/software/automake/manual/html_node/Multiple-Outputs.html
            timestamp_var_name = "%s_ALL" % request.name.upper()
            timestamp_file = TmpFile("%s.timestamp" % request.name)
            rules = [
                MakeFilesVar(
                    name = timestamp_var_name,
                    files = [timestamp_file]
                ),
                MakeRule(
                    name = "%s_all" % request.name,
                    dep_literals = [],
                    dep_files = dep_files,
                    output_file = timestamp_file,
                    cmds = [
                        cmd,
                        "echo timestamp > {MAKEFILENAME}".format(
                            MAKEFILENAME = files_to_makefile([timestamp_file], common_vars)
                        )
                    ]
                )
            ]
            for i, file in enumerate(request.output_files):
                rules += [
                    MakeRule(
                        name = "%s_%d" % (request.name, i),
                        dep_literals = ["$(%s)" % timestamp_var_name],
                        dep_files = [],
                        output_file = file,
                        cmds = []
                    )
                ]
            return rules

        elif len(dep_files) > 5:
            # For nicer printing, for long input lists, use a helper variable.
            dep_var_name = "%s_DEPS" % request.name.upper()
            return [
                MakeFilesVar(
                    name = dep_var_name,
                    files = dep_files
                ),
                MakeRule(
                    name = request.name,
                    dep_literals = ["$(%s)" % dep_var_name],
                    dep_files = [],
                    output_file = request.output_files[0],
                    cmds = [cmd]
                )
            ]

        else:
            return [
                MakeRule(
                    name = request.name,
                    dep_literals = [],
                    dep_files = dep_files,
                    output_file = request.output_files[0],
                    cmds = [cmd]
                )
            ]

    if isinstance(request, RepeatedExecutionRequest):
        rules = []
        dep_literals = []
        # To keep from repeating the same dep files many times, make a variable.
        if len(request.common_dep_files) > 0:
            dep_var_name = "%s_DEPS" % request.name.upper()
            dep_literals = ["$(%s)" % dep_var_name]
            rules += [
                MakeFilesVar(
                    name = dep_var_name,
                    files = request.common_dep_files
                )
            ]
        # Add a rule for each individual file.
        for loop_vars in utils.repeated_execution_request_looper(request):
            (_, specific_dep_files, input_file, output_file) = loop_vars
            name_suffix = input_file[input_file.filename.rfind("/")+1:input_file.filename.rfind(".")]
            cmd = utils.format_repeated_request_command(
                request,
                cmd_template,
                loop_vars,
                common_vars
            )
            rules += [
                MakeRule(
                    name = "%s_%s" % (request.name, name_suffix),
                    dep_literals = dep_literals,
                    dep_files = specific_dep_files + [input_file],
                    output_file = output_file,
                    cmds = [cmd]
                )
            ]
        return rules

    assert False
