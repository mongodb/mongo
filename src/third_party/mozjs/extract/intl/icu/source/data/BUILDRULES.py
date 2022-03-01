# Copyright (C) 2018 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html

# Python 2/3 Compatibility (ICU-20299)
# TODO(ICU-20301): Remove this.
from __future__ import print_function

from icutools.databuilder import *
from icutools.databuilder import utils
from icutools.databuilder.request_types import *

import os
import sys


def generate(config, io, common_vars):
    requests = []

    if len(io.glob("misc/*")) == 0:
        print("Error: Cannot find data directory; please specify --src_dir", file=sys.stderr)
        exit(1)

    requests += generate_cnvalias(config, io, common_vars)
    requests += generate_ulayout(config, io, common_vars)
    requests += generate_confusables(config, io, common_vars)
    requests += generate_conversion_mappings(config, io, common_vars)
    requests += generate_brkitr_brk(config, io, common_vars)
    requests += generate_stringprep(config, io, common_vars)
    requests += generate_brkitr_dictionaries(config, io, common_vars)
    requests += generate_normalization(config, io, common_vars)
    requests += generate_coll_ucadata(config, io, common_vars)
    requests += generate_full_unicore_data(config, io, common_vars)
    requests += generate_unames(config, io, common_vars)
    requests += generate_misc(config, io, common_vars)
    requests += generate_curr_supplemental(config, io, common_vars)
    requests += generate_zone_supplemental(config, io, common_vars)
    requests += generate_translit(config, io, common_vars)

    # Res Tree Files
    # (input dirname, output dirname, resfiles.mk path, mk version var, mk source var, use pool file, dep files)
    requests += generate_tree(config, io, common_vars,
        "locales",
        None,
        config.use_pool_bundle,
        [])

    requests += generate_tree(config, io, common_vars,
        "curr",
        "curr",
        config.use_pool_bundle,
        [])

    requests += generate_tree(config, io, common_vars,
        "lang",
        "lang",
        config.use_pool_bundle,
        [])

    requests += generate_tree(config, io, common_vars,
        "region",
        "region",
        config.use_pool_bundle,
        [])

    requests += generate_tree(config, io, common_vars,
        "zone",
        "zone",
        config.use_pool_bundle,
        [])

    requests += generate_tree(config, io, common_vars,
        "unit",
        "unit",
        config.use_pool_bundle,
        [])

    requests += generate_tree(config, io, common_vars,
        "coll",
        "coll",
        # Never use pool bundle for coll, brkitr, or rbnf
        False,
        # Depends on timezoneTypes.res and keyTypeData.res.
        # TODO: We should not need this dependency to build collation.
        # TODO: Bake keyTypeData.res into the common library?
        [DepTarget("coll_ucadata"), DepTarget("misc_res"), InFile("unidata/UCARules.txt")])

    requests += generate_tree(config, io, common_vars,
        "brkitr",
        "brkitr",
        # Never use pool bundle for coll, brkitr, or rbnf
        False,
        [DepTarget("brkitr_brk"), DepTarget("dictionaries")])

    requests += generate_tree(config, io, common_vars,
        "rbnf",
        "rbnf",
        # Never use pool bundle for coll, brkitr, or rbnf
        False,
        [])

    requests += [
        ListRequest(
            name = "icudata_list",
            variable_name = "icudata_all_output_files",
            output_file = TmpFile("icudata.lst"),
            include_tmp = False
        )
    ]

    return requests


def generate_cnvalias(config, io, common_vars):
    # UConv Name Aliases
    input_file = InFile("mappings/convrtrs.txt")
    output_file = OutFile("cnvalias.icu")
    return [
        SingleExecutionRequest(
            name = "cnvalias",
            category = "cnvalias",
            dep_targets = [],
            input_files = [input_file],
            output_files = [output_file],
            tool = IcuTool("gencnval"),
            args = "-s {IN_DIR} -d {OUT_DIR} "
                "{INPUT_FILES[0]}",
            format_with = {}
        )
    ]


def generate_confusables(config, io, common_vars):
    # CONFUSABLES
    txt1 = InFile("unidata/confusables.txt")
    txt2 = InFile("unidata/confusablesWholeScript.txt")
    cfu = OutFile("confusables.cfu")
    return [
        SingleExecutionRequest(
            name = "confusables",
            category = "confusables",
            dep_targets = [DepTarget("cnvalias")],
            input_files = [txt1, txt2],
            output_files = [cfu],
            tool = IcuTool("gencfu"),
            args = "-d {OUT_DIR} -i {OUT_DIR} "
                "-c -r {IN_DIR}/{INPUT_FILES[0]} -w {IN_DIR}/{INPUT_FILES[1]} "
                "-o {OUTPUT_FILES[0]}",
            format_with = {}
        )
    ]


def generate_conversion_mappings(config, io, common_vars):
    # UConv Conversion Table Files
    input_files = [InFile(filename) for filename in io.glob("mappings/*.ucm")]
    output_files = [OutFile("%s.cnv" % v.filename[9:-4]) for v in input_files]
    # TODO: handle BUILD_SPECIAL_CNV_FILES? Means to add --ignore-siso-check flag to makeconv
    return [
        RepeatedOrSingleExecutionRequest(
            name = "conversion_mappings",
            category = "conversion_mappings",
            dep_targets = [],
            input_files = input_files,
            output_files = output_files,
            tool = IcuTool("makeconv"),
            args = "-s {IN_DIR} -d {OUT_DIR} -c {INPUT_FILE_PLACEHOLDER}",
            format_with = {},
            repeat_with = {
                "INPUT_FILE_PLACEHOLDER": utils.SpaceSeparatedList(file.filename for file in input_files)
            }
        )
    ]


def generate_brkitr_brk(config, io, common_vars):
    # BRK Files
    input_files = [InFile(filename) for filename in io.glob("brkitr/rules/*.txt")]
    output_files = [OutFile("brkitr/%s.brk" % v.filename[13:-4]) for v in input_files]
    return [
        RepeatedExecutionRequest(
            name = "brkitr_brk",
            category = "brkitr_rules",
            dep_targets = [DepTarget("cnvalias"), DepTarget("ulayout")],
            input_files = input_files,
            output_files = output_files,
            tool = IcuTool("genbrk"),
            args = "-d {OUT_DIR} -i {OUT_DIR} "
                "-c -r {IN_DIR}/{INPUT_FILE} "
                "-o {OUTPUT_FILE}",
            format_with = {},
            repeat_with = {}
        )
    ]


def generate_stringprep(config, io, common_vars):
    # SPP FILES
    input_files = [InFile(filename) for filename in io.glob("sprep/*.txt")]
    output_files = [OutFile("%s.spp" % v.filename[6:-4]) for v in input_files]
    bundle_names = [v.filename[6:-4] for v in input_files]
    return [
        RepeatedExecutionRequest(
            name = "stringprep",
            category = "stringprep",
            dep_targets = [InFile("unidata/NormalizationCorrections.txt")],
            input_files = input_files,
            output_files = output_files,
            tool = IcuTool("gensprep"),
            args = "-s {IN_DIR}/sprep -d {OUT_DIR} -i {OUT_DIR} "
                "-b {BUNDLE_NAME} -m {IN_DIR}/unidata -u 3.2.0 {BUNDLE_NAME}.txt",
            format_with = {},
            repeat_with = {
                "BUNDLE_NAME": bundle_names
            }
        )
    ]


def generate_brkitr_dictionaries(config, io, common_vars):
    # Dict Files
    input_files = [InFile(filename) for filename in io.glob("brkitr/dictionaries/*.txt")]
    output_files = [OutFile("brkitr/%s.dict" % v.filename[20:-4]) for v in input_files]
    extra_options_map = {
        "brkitr/dictionaries/burmesedict.txt": "--bytes --transform offset-0x1000",
        "brkitr/dictionaries/cjdict.txt": "--uchars",
        "brkitr/dictionaries/khmerdict.txt": "--bytes --transform offset-0x1780",
        "brkitr/dictionaries/laodict.txt": "--bytes --transform offset-0x0e80",
        "brkitr/dictionaries/thaidict.txt": "--bytes --transform offset-0x0e00"
    }
    extra_optionses = [extra_options_map[v.filename] for v in input_files]
    return [
        RepeatedExecutionRequest(
            name = "dictionaries",
            category = "brkitr_dictionaries",
            dep_targets = [],
            input_files = input_files,
            output_files = output_files,
            tool = IcuTool("gendict"),
            args = "-i {OUT_DIR} "
                "-c {EXTRA_OPTIONS} "
                "{IN_DIR}/{INPUT_FILE} {OUT_DIR}/{OUTPUT_FILE}",
            format_with = {},
            repeat_with = {
                "EXTRA_OPTIONS": extra_optionses
            }
        )
    ]


def generate_normalization(config, io, common_vars):
    # NRM Files
    input_files = [InFile(filename) for filename in io.glob("in/*.nrm")]
    # nfc.nrm is pre-compiled into C++; see generate_full_unicore_data
    input_files.remove(InFile("in/nfc.nrm"))
    output_files = [OutFile(v.filename[3:]) for v in input_files]
    return [
        RepeatedExecutionRequest(
            name = "normalization",
            category = "normalization",
            dep_targets = [],
            input_files = input_files,
            output_files = output_files,
            tool = IcuTool("icupkg"),
            args = "-t{ICUDATA_CHAR} {IN_DIR}/{INPUT_FILE} {OUT_DIR}/{OUTPUT_FILE}",
            format_with = {},
            repeat_with = {}
        )
    ]


def generate_coll_ucadata(config, io, common_vars):
    # Collation Dependency File (ucadata.icu)
    input_file = InFile("in/coll/ucadata-%s.icu" % config.coll_han_type)
    output_file = OutFile("coll/ucadata.icu")
    return [
        SingleExecutionRequest(
            name = "coll_ucadata",
            category = "coll_ucadata",
            dep_targets = [],
            input_files = [input_file],
            output_files = [output_file],
            tool = IcuTool("icupkg"),
            args = "-t{ICUDATA_CHAR} {IN_DIR}/{INPUT_FILES[0]} {OUT_DIR}/{OUTPUT_FILES[0]}",
            format_with = {}
        )
    ]


def generate_full_unicore_data(config, io, common_vars):
    # The core Unicode properties files (pnames.icu, uprops.icu, ucase.icu, ubidi.icu)
    # are hardcoded in the common DLL and therefore not included in the data package any more.
    # They are not built by default but need to be built for ICU4J data,
    # both in the .jar and in the .dat file (if ICU4J uses the .dat file).
    # See ICU-4497.
    if not config.include_uni_core_data:
        return []

    basenames = [
        "pnames.icu",
        "uprops.icu",
        "ucase.icu",
        "ubidi.icu",
        "nfc.nrm"
    ]
    input_files = [InFile("in/%s" % bn) for bn in basenames]
    output_files = [OutFile(bn) for bn in basenames]
    return [
        RepeatedExecutionRequest(
            name = "unicore",
            category = "unicore",
            input_files = input_files,
            output_files = output_files,
            tool = IcuTool("icupkg"),
            args = "-t{ICUDATA_CHAR} {IN_DIR}/{INPUT_FILE} {OUT_DIR}/{OUTPUT_FILE}"
        )
    ]


def generate_unames(config, io, common_vars):
    # Unicode Character Names
    input_file = InFile("in/unames.icu")
    output_file = OutFile("unames.icu")
    return [
        SingleExecutionRequest(
            name = "unames",
            category = "unames",
            dep_targets = [],
            input_files = [input_file],
            output_files = [output_file],
            tool = IcuTool("icupkg"),
            args = "-t{ICUDATA_CHAR} {IN_DIR}/{INPUT_FILES[0]} {OUT_DIR}/{OUTPUT_FILES[0]}",
            format_with = {}
        )
    ]


def generate_ulayout(config, io, common_vars):
    # Unicode text layout properties
    basename = "ulayout"
    input_file = InFile("in/%s.icu" % basename)
    output_file = OutFile("%s.icu" % basename)
    return [
        SingleExecutionRequest(
            name = basename,
            category = basename,
            dep_targets = [],
            input_files = [input_file],
            output_files = [output_file],
            tool = IcuTool("icupkg"),
            args = "-t{ICUDATA_CHAR} {IN_DIR}/{INPUT_FILES[0]} {OUT_DIR}/{OUTPUT_FILES[0]}",
            format_with = {}
        )
    ]


def generate_misc(config, io, common_vars):
    # Misc Data Res Files
    input_files = [InFile(filename) for filename in io.glob("misc/*.txt")]
    input_basenames = [v.filename[5:] for v in input_files]
    output_files = [OutFile("%s.res" % v[:-4]) for v in input_basenames]
    return [
        RepeatedExecutionRequest(
            name = "misc_res",
            category = "misc",
            dep_targets = [DepTarget("cnvalias")], # ICU-21175
            input_files = input_files,
            output_files = output_files,
            tool = IcuTool("genrb"),
            args = "-s {IN_DIR}/misc -d {OUT_DIR} -i {OUT_DIR} "
                "-k -q "
                "{INPUT_BASENAME}",
            format_with = {},
            repeat_with = {
                "INPUT_BASENAME": input_basenames
            }
        )
    ]


def generate_curr_supplemental(config, io, common_vars):
    # Currency Supplemental Res File
    input_file = InFile("curr/supplementalData.txt")
    input_basename = "supplementalData.txt"
    output_file = OutFile("curr/supplementalData.res")
    return [
        SingleExecutionRequest(
            name = "curr_supplemental_res",
            category = "curr_supplemental",
            dep_targets = [],
            input_files = [input_file],
            output_files = [output_file],
            tool = IcuTool("genrb"),
            args = "-s {IN_DIR}/curr -d {OUT_DIR}/curr -i {OUT_DIR} "
                "-k "
                "{INPUT_BASENAME}",
            format_with = {
                "INPUT_BASENAME": input_basename
            }
        )
    ]


def generate_zone_supplemental(config, io, common_vars):
    # tzdbNames Res File
    input_file = InFile("zone/tzdbNames.txt")
    input_basename = "tzdbNames.txt"
    output_file = OutFile("zone/tzdbNames.res")
    return [
        SingleExecutionRequest(
            name = "zone_supplemental_res",
            category = "zone_supplemental",
            dep_targets = [],
            input_files = [input_file],
            output_files = [output_file],
            tool = IcuTool("genrb"),
            args = "-s {IN_DIR}/zone -d {OUT_DIR}/zone -i {OUT_DIR} "
                "-k "
                "{INPUT_BASENAME}",
            format_with = {
                "INPUT_BASENAME": input_basename
            }
        )
    ]


def generate_translit(config, io, common_vars):
    input_files = [
        InFile("translit/root.txt"),
        InFile("translit/en.txt"),
        InFile("translit/el.txt")
    ]
    dep_files = set(InFile(filename) for filename in io.glob("translit/*.txt"))
    dep_files -= set(input_files)
    dep_files = list(sorted(dep_files))
    input_basenames = [v.filename[9:] for v in input_files]
    output_files = [
        OutFile("translit/%s.res" % v[:-4])
        for v in input_basenames
    ]
    return [
        RepeatedOrSingleExecutionRequest(
            name = "translit_res",
            category = "translit",
            dep_targets = dep_files,
            input_files = input_files,
            output_files = output_files,
            tool = IcuTool("genrb"),
            args = "-s {IN_DIR}/translit -d {OUT_DIR}/translit -i {OUT_DIR} "
                "-k "
                "{INPUT_BASENAME}",
            format_with = {
            },
            repeat_with = {
                "INPUT_BASENAME": utils.SpaceSeparatedList(input_basenames)
            }
        )
    ]


def generate_tree(
        config,
        io,
        common_vars,
        sub_dir,
        out_sub_dir,
        use_pool_bundle,
        dep_targets):
    requests = []
    category = "%s_tree" % sub_dir
    out_prefix = "%s/" % out_sub_dir if out_sub_dir else ""
    input_files = [InFile(filename) for filename in io.glob("%s/*.txt" % sub_dir)]
    if sub_dir == "curr":
        input_files.remove(InFile("curr/supplementalData.txt"))
    if sub_dir == "zone":
        input_files.remove(InFile("zone/tzdbNames.txt"))
    input_basenames = [v.filename[len(sub_dir)+1:] for v in input_files]
    output_files = [
        OutFile("%s%s.res" % (out_prefix, v[:-4]))
        for v in input_basenames
    ]

    # Generate Pool Bundle
    if use_pool_bundle:
        input_pool_files = [OutFile("%spool.res" % out_prefix)]
        pool_target_name = "%s_pool_write" % sub_dir
        use_pool_bundle_option = "--usePoolBundle {OUT_DIR}/{OUT_PREFIX}".format(
            OUT_PREFIX = out_prefix,
            **common_vars
        )
        requests += [
            SingleExecutionRequest(
                name = pool_target_name,
                category = category,
                dep_targets = dep_targets,
                input_files = input_files,
                output_files = input_pool_files,
                tool = IcuTool("genrb"),
                args = "-s {IN_DIR}/{IN_SUB_DIR} -d {OUT_DIR}/{OUT_PREFIX} -i {OUT_DIR} "
                    "--writePoolBundle -k "
                    "{INPUT_BASENAMES_SPACED}",
                format_with = {
                    "IN_SUB_DIR": sub_dir,
                    "OUT_PREFIX": out_prefix,
                    "INPUT_BASENAMES_SPACED": utils.SpaceSeparatedList(input_basenames)
                }
            ),
        ]
        dep_targets = dep_targets + [DepTarget(pool_target_name)]
    else:
        use_pool_bundle_option = ""

    # Generate Res File Tree
    requests += [
        RepeatedOrSingleExecutionRequest(
            name = "%s_res" % sub_dir,
            category = category,
            dep_targets = dep_targets,
            input_files = input_files,
            output_files = output_files,
            tool = IcuTool("genrb"),
            args = "-s {IN_DIR}/{IN_SUB_DIR} -d {OUT_DIR}/{OUT_PREFIX} -i {OUT_DIR} "
                "{EXTRA_OPTION} -k "
                "{INPUT_BASENAME}",
            format_with = {
                "IN_SUB_DIR": sub_dir,
                "OUT_PREFIX": out_prefix,
                "EXTRA_OPTION": use_pool_bundle_option
            },
            repeat_with = {
                "INPUT_BASENAME": utils.SpaceSeparatedList(input_basenames)
            }
        )
    ]

    # Generate res_index file
    # Exclude the deprecated locale variants and root; see ICU-20628. This
    # could be data-driven, but we do not want to perform I/O in this script
    # (for example, we do not want to read from an XML file).
    excluded_locales = set([
        "ja_JP_TRADITIONAL",
        "th_TH_TRADITIONAL",
        "de_",
        "de__PHONEBOOK",
        "es_",
        "es__TRADITIONAL",
        "root",
    ])
    # Put alias locales in a separate structure; see ICU-20627
    dependency_data = io.read_locale_deps(sub_dir)
    if "aliases" in dependency_data:
        alias_locales = set(dependency_data["aliases"].keys())
    else:
        alias_locales = set()
    alias_files = []
    installed_files = []
    for f in input_files:
        file_stem = IndexRequest.locale_file_stem(f)
        if file_stem in excluded_locales:
            continue
        destination = alias_files if file_stem in alias_locales else installed_files
        destination.append(f)
    cldr_version = dependency_data["cldrVersion"] if sub_dir == "locales" else None
    index_file_txt = TmpFile("{IN_SUB_DIR}/{INDEX_NAME}.txt".format(
        IN_SUB_DIR = sub_dir,
        **common_vars
    ))
    index_res_file = OutFile("{OUT_PREFIX}{INDEX_NAME}.res".format(
        OUT_PREFIX = out_prefix,
        **common_vars
    ))
    index_file_target_name = "%s_index_txt" % sub_dir
    requests += [
        IndexRequest(
            name = index_file_target_name,
            category = category,
            installed_files = installed_files,
            alias_files = alias_files,
            txt_file = index_file_txt,
            output_file = index_res_file,
            cldr_version = cldr_version,
            args = "-s {TMP_DIR}/{IN_SUB_DIR} -d {OUT_DIR}/{OUT_PREFIX} -i {OUT_DIR} "
                "-k "
                "{INDEX_NAME}.txt",
            format_with = {
                "IN_SUB_DIR": sub_dir,
                "OUT_PREFIX": out_prefix
            }
        )
    ]

    return requests
