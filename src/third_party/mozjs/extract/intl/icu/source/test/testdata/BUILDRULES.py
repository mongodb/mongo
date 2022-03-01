# Copyright (C) 2018 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html

from icutools.databuilder import *
from icutools.databuilder.request_types import *


def generate(config, io, common_vars):
    requests = []
    requests += generate_rb(config, io, common_vars)
    requests += generate_sprep(config, io, common_vars)
    requests += generate_conv(config, io, common_vars)
    requests += generate_other(config, io, common_vars)
    requests += generate_copy(config, io, common_vars)

    requests += [
        ListRequest(
            name = "testdata_list",
            variable_name = "testdata_all_output_files",
            output_file = TmpFile("testdata.lst"),
            include_tmp = True
        )
    ]

    return requests


def generate_rb(config, io, common_vars):
    basenames = [
        "calendar",
        "casing",
        "conversion",
        "format",
        "icuio",
        "idna_rules",
        "mc",
        "root",
        "sh_YU",
        "sh",
        "structLocale",
        "te_IN_REVISED",
        "te_IN",
        "te",
        "testaliases",
        "testempty",
        "testtypes",
        # "metaZones",
        # "timezoneTypes",
        # "windowsZones",
    ]
    return [
        # Inference rule for creating resource bundles
        # Some test data resource bundles are known to have warnings and bad data.
        # The -q option is there on purpose, so we don't see it normally.
        # TODO: Use option -k?
        RepeatedExecutionRequest(
            name = "testrb",
            category = "tests",
            input_files = [InFile("%s.txt" % bn) for bn in basenames],
            output_files = [OutFile("%s.res" % bn) for bn in basenames],
            tool = IcuTool("genrb"),
            args = "-q -s {IN_DIR} -d {OUT_DIR} {INPUT_FILE}",
            format_with = {},
            repeat_with = {}
        ),
        # Other standalone res files
        SingleExecutionRequest(
            name = "encoded",
            category = "tests",
            input_files = [InFile("encoded.utf16be")],
            output_files = [OutFile("encoded.res")],
            tool = IcuTool("genrb"),
            args = "-s {IN_DIR} -eUTF-16BE -d {OUT_DIR} {INPUT_FILES[0]}",
            format_with = {}
        ),
        SingleExecutionRequest(
            name = "zoneinfo64",
            category = "tests",
            input_files = [InFile("zoneinfo64.txt")],
            output_files = [TmpFile("zoneinfo64.res")],
            tool = IcuTool("genrb"),
            args = "-s {IN_DIR} -d {TMP_DIR} {INPUT_FILES[0]}",
            format_with = {}
        ),
        SingleExecutionRequest(
            name = "filtertest",
            category = "tests",
            input_files = [InFile("filtertest.txt")],
            output_files = [OutFile("filtertest.res")],
            tool = IcuTool("genrb"),
            args = "-s {IN_DIR} -d {OUT_DIR} -i {OUT_DIR} "
                "--filterDir {IN_DIR}/filters filtertest.txt",
            format_with = {}
        )
    ]


def generate_sprep(config, io, common_vars):
    return [
        SingleExecutionRequest(
            name = "nfscsi",
            category = "tests",
            input_files = [InFile("nfs4_cs_prep_ci.txt")],
            output_files = [OutFile("nfscsi.spp")],
            tool = IcuTool("gensprep"),
            args = "-s {IN_DIR} -d {OUT_DIR} -b nfscsi -u 3.2.0 {INPUT_FILES[0]}",
            format_with = {}
        ),
        SingleExecutionRequest(
            name = "nfscss",
            category = "tests",
            input_files = [InFile("nfs4_cs_prep_cs.txt")],
            output_files = [OutFile("nfscss.spp")],
            tool = IcuTool("gensprep"),
            args = "-s {IN_DIR} -d {OUT_DIR} -b nfscss -u 3.2.0 {INPUT_FILES[0]}",
            format_with = {}
        ),
        SingleExecutionRequest(
            name = "nfscis",
            category = "tests",
            input_files = [InFile("nfs4_cis_prep.txt")],
            output_files = [OutFile("nfscis.spp")],
            tool = IcuTool("gensprep"),
            args = "-s {IN_DIR} -d {OUT_DIR} -b nfscis -u 3.2.0 -k -n {IN_DIR}/../../data/unidata {INPUT_FILES[0]}",
            format_with = {}
        ),
        SingleExecutionRequest(
            name = "nfsmxs",
            category = "tests",
            input_files = [InFile("nfs4_mixed_prep_s.txt")],
            output_files = [OutFile("nfsmxs.spp")],
            tool = IcuTool("gensprep"),
            args = "-s {IN_DIR} -d {OUT_DIR} -b nfsmxs -u 3.2.0 -k -n {IN_DIR}/../../data/unidata {INPUT_FILES[0]}",
            format_with = {}
        ),
        SingleExecutionRequest(
            name = "nfsmxp",
            category = "tests",
            input_files = [InFile("nfs4_mixed_prep_p.txt")],
            output_files = [OutFile("nfsmxp.spp")],
            tool = IcuTool("gensprep"),
            args = "-s {IN_DIR} -d {OUT_DIR} -b nfsmxp -u 3.2.0 -k -n {IN_DIR}/../../data/unidata {INPUT_FILES[0]}",
            format_with = {}
        )
    ]


def generate_conv(config, io, common_vars):
    basenames = [
        "test1",
        "test1bmp",
        "test2",
        "test3",
        "test4",
        "test4x",
        "test5",
        "ibm9027"
    ]
    return [
        RepeatedExecutionRequest(
            name = "test_conv",
            category = "tests",
            input_files = [InFile("%s.ucm" % bn) for bn in basenames],
            output_files = [OutFile("%s.cnv" % bn) for bn in basenames],
            tool = IcuTool("makeconv"),
            args = "--small -d {OUT_DIR} {IN_DIR}/{INPUT_FILE}",
            format_with = {},
            repeat_with = {}
        )
    ]


def generate_copy(config, io, common_vars):
    return [
        CopyRequest(
            name = "nam_typ",
            input_file = OutFile("te.res"),
            output_file = TmpFile("nam.typ")
        ),
        CopyRequest(
            name = "old_l_testtypes",
            input_file = InFile("old_l_testtypes.res"),
            output_file = OutFile("old_l_testtypes.res")
        ),
        CopyRequest(
            name = "old_e_testtypes",
            input_file = InFile("old_e_testtypes.res"),
            output_file = OutFile("old_e_testtypes.res")
        ),
    ]


def generate_other(config, io, common_vars):
    return [
        SingleExecutionRequest(
            name = "testnorm",
            category = "tests",
            input_files = [InFile("testnorm.txt")],
            output_files = [OutFile("testnorm.nrm")],
            tool = IcuTool("gennorm2"),
            args = "-s {IN_DIR} {INPUT_FILES[0]} -o {OUT_DIR}/{OUTPUT_FILES[0]}",
            format_with = {}
        ),
        SingleExecutionRequest(
            name = "test_icu",
            category = "tests",
            input_files = [],
            output_files = [OutFile("test.icu")],
            tool = IcuTool("gentest"),
            args = "-d {OUT_DIR}",
            format_with = {}
        ),
        SingleExecutionRequest(
            name = "testtable32_txt",
            category = "tests",
            input_files = [],
            output_files = [TmpFile("testtable32.txt")],
            tool = IcuTool("gentest"),
            args = "-r -d {TMP_DIR}",
            format_with = {}
        ),
        SingleExecutionRequest(
            name = "testtable32_res",
            category = "tests",
            input_files = [TmpFile("testtable32.txt")],
            output_files = [OutFile("testtable32.res")],
            tool = IcuTool("genrb"),
            args = "-s {TMP_DIR} -d {OUT_DIR} {INPUT_FILES[0]}",
            format_with = {}
        )
    ]
