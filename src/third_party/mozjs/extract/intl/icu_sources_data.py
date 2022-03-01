#!/usr/bin/env python
#
# Any copyright is dedicated to the Public Domain.
# http://creativecommons.org/publicdomain/zero/1.0/
#
# Generate SOURCES in sources.mozbuild files from ICU's Makefile.in
# files, and also build a standalone copy of ICU using its build
# system to generate a new copy of the in-tree ICU data file.
#
# This script expects to be run from `update-icu.sh` after the in-tree
# copy of ICU has been updated.

from __future__ import absolute_import
from __future__ import print_function

import glob
import multiprocessing
import os
import shutil
import subprocess
import sys
import tempfile

from mozpack import path as mozpath

# The following files have been determined to be dead/unused by a
# semi-automated analysis. You can just remove any of the files below
# if you need them. However, files marked with a "Cluster" comment
# can only be removed together, as they have (directional) dependencies.
# If you want to rerun this analysis, contact :decoder.
UNUSED_SOURCES = set(
    [
        "intl/icu/source/common/bytestrieiterator.cpp",
        "intl/icu/source/common/cstr.cpp",
        "intl/icu/source/common/cwchar.cpp",
        "intl/icu/source/common/icudataver.cpp",
        "intl/icu/source/common/icuplug.cpp",
        "intl/icu/source/common/pluralmap.cpp",
        "intl/icu/source/common/ucat.cpp",
        "intl/icu/source/common/ucnv2022.cpp",
        "intl/icu/source/common/ucnv_ct.cpp",
        "intl/icu/source/common/ucnvdisp.cpp",
        "intl/icu/source/common/ucnv_ext.cpp",
        "intl/icu/source/common/ucnvhz.cpp",
        "intl/icu/source/common/ucnvisci.cpp",
        "intl/icu/source/common/ucnv_lmb.cpp",
        "intl/icu/source/common/ucnvmbcs.cpp",
        "intl/icu/source/common/uidna.cpp",
        "intl/icu/source/common/unorm.cpp",
        "intl/icu/source/common/usc_impl.cpp",
        "intl/icu/source/common/ustr_wcs.cpp",
        "intl/icu/source/common/util_props.cpp",
        "intl/icu/source/i18n/anytrans.cpp",
        "intl/icu/source/i18n/brktrans.cpp",
        "intl/icu/source/i18n/casetrn.cpp",
        "intl/icu/source/i18n/cpdtrans.cpp",
        "intl/icu/source/i18n/esctrn.cpp",
        "intl/icu/source/i18n/fmtable_cnv.cpp",
        "intl/icu/source/i18n/funcrepl.cpp",
        "intl/icu/source/i18n/gender.cpp",
        "intl/icu/source/i18n/name2uni.cpp",
        "intl/icu/source/i18n/nortrans.cpp",
        "intl/icu/source/i18n/nultrans.cpp",
        "intl/icu/source/i18n/quant.cpp",
        "intl/icu/source/i18n/rbt.cpp",
        "intl/icu/source/i18n/rbt_data.cpp",
        "intl/icu/source/i18n/rbt_pars.cpp",
        "intl/icu/source/i18n/rbt_rule.cpp",
        "intl/icu/source/i18n/rbt_set.cpp",
        "intl/icu/source/i18n/regexcmp.cpp",
        "intl/icu/source/i18n/regeximp.cpp",
        "intl/icu/source/i18n/regexst.cpp",
        "intl/icu/source/i18n/regextxt.cpp",
        "intl/icu/source/i18n/rematch.cpp",
        "intl/icu/source/i18n/remtrans.cpp",
        "intl/icu/source/i18n/repattrn.cpp",
        "intl/icu/source/i18n/scientificnumberformatter.cpp",
        "intl/icu/source/i18n/strmatch.cpp",
        "intl/icu/source/i18n/strrepl.cpp",
        "intl/icu/source/i18n/titletrn.cpp",
        "intl/icu/source/i18n/tolowtrn.cpp",
        "intl/icu/source/i18n/toupptrn.cpp",
        "intl/icu/source/i18n/translit.cpp",
        "intl/icu/source/i18n/transreg.cpp",
        "intl/icu/source/i18n/tridpars.cpp",
        "intl/icu/source/i18n/unesctrn.cpp",
        "intl/icu/source/i18n/uni2name.cpp",
        "intl/icu/source/i18n/uregexc.cpp",
        "intl/icu/source/i18n/uregex.cpp",
        "intl/icu/source/i18n/uregion.cpp",
        "intl/icu/source/i18n/uspoof_build.cpp",
        "intl/icu/source/i18n/uspoof_conf.cpp",
        "intl/icu/source/i18n/utrans.cpp",
        "intl/icu/source/i18n/vzone.cpp",
        "intl/icu/source/i18n/zrule.cpp",
        "intl/icu/source/i18n/ztrans.cpp",
        # Cluster
        "intl/icu/source/common/resbund_cnv.cpp",
        "intl/icu/source/common/ures_cnv.cpp",
        # Cluster
        "intl/icu/source/common/propsvec.cpp",
        "intl/icu/source/common/ucnvsel.cpp",
        "intl/icu/source/common/ucnv_set.cpp",
        # Cluster
        "intl/icu/source/common/ubiditransform.cpp",
        "intl/icu/source/common/ushape.cpp",
        # Cluster
        "intl/icu/source/i18n/csdetect.cpp",
        "intl/icu/source/i18n/csmatch.cpp",
        "intl/icu/source/i18n/csr2022.cpp",
        "intl/icu/source/i18n/csrecog.cpp",
        "intl/icu/source/i18n/csrmbcs.cpp",
        "intl/icu/source/i18n/csrsbcs.cpp",
        "intl/icu/source/i18n/csrucode.cpp",
        "intl/icu/source/i18n/csrutf8.cpp",
        "intl/icu/source/i18n/inputext.cpp",
        "intl/icu/source/i18n/ucsdet.cpp",
        # Cluster
        "intl/icu/source/i18n/alphaindex.cpp",
        "intl/icu/source/i18n/ulocdata.cpp",
    ]
)


def ensure_source_file_exists(dir, filename):
    f = mozpath.join(dir, filename)
    if os.path.isfile(f):
        return f
    raise Exception("Couldn't find source file for: %s" % filename)


def get_sources(sources_file):
    srcdir = os.path.dirname(sources_file)
    with open(sources_file) as f:
        return sorted(
            (ensure_source_file_exists(srcdir, name.strip()) for name in f),
            key=lambda x: x.lower(),
        )


def list_headers(path):
    result = []
    for name in os.listdir(path):
        f = mozpath.join(path, name)
        if os.path.isfile(f):
            result.append(f)
    return sorted(result, key=lambda x: x.lower())


def write_sources(mozbuild, sources, headers):
    with open(mozbuild, "w", newline="\n", encoding="utf-8") as f:
        f.write(
            "# THIS FILE IS GENERATED BY /intl/icu_sources_data.py " + "DO NOT EDIT\n"
        )

        def write_list(name, content):
            if content:
                f.write("%s %s [\n" % (name, "=" if name.islower() else "+="))
                f.write("".join("   '/%s',\n" % s for s in content))
                f.write("]\n")

        write_list("sources", [s for s in sources if s not in UNUSED_SOURCES])
        write_list("other_sources", [s for s in sources if s in UNUSED_SOURCES])
        write_list("EXPORTS.unicode", headers)


def update_sources(topsrcdir):
    print("Updating ICU sources lists...")
    for d in ["common", "i18n", "tools/toolutil", "tools/icupkg"]:
        base_path = mozpath.join(topsrcdir, "intl/icu/source/%s" % d)
        sources_file = mozpath.join(base_path, "sources.txt")
        mozbuild = mozpath.join(
            topsrcdir, "config/external/icu/%s/sources.mozbuild" % mozpath.basename(d)
        )
        sources = [mozpath.relpath(s, topsrcdir) for s in get_sources(sources_file)]
        unicode_dir = mozpath.join(base_path, "unicode")
        if os.path.exists(unicode_dir):
            headers = [
                mozpath.normsep(os.path.relpath(s, topsrcdir))
                for s in list_headers(unicode_dir)
            ]
        else:
            headers = None
        write_sources(mozbuild, sources, headers)


def try_run(name, command, cwd=None, **kwargs):
    try:
        with tempfile.NamedTemporaryFile(prefix=name, delete=False) as f:
            subprocess.check_call(
                command, cwd=cwd, stdout=f, stderr=subprocess.STDOUT, **kwargs
            )
    except subprocess.CalledProcessError:
        print(
            """Error running "{}" in directory {}
    See output in {}""".format(
                " ".join(command), cwd, f.name
            ),
            file=sys.stderr,
        )
        return False
    else:
        os.unlink(f.name)
        return True


def get_data_file(data_dir):
    files = glob.glob(mozpath.join(data_dir, "icudt*.dat"))
    return files[0] if files else None


def update_data_file(topsrcdir):
    objdir = tempfile.mkdtemp(prefix="icu-obj-")
    configure = mozpath.join(topsrcdir, "intl/icu/source/configure")
    env = dict(os.environ)
    # bug 1262101 - these should be shared with the moz.build files
    env.update(
        {
            "CPPFLAGS": (
                "-DU_NO_DEFAULT_INCLUDE_UTF_HEADERS=1 "
                + "-DU_HIDE_OBSOLETE_UTF_OLD_H=1"
                + "-DUCONFIG_NO_LEGACY_CONVERSION "
                + "-DUCONFIG_NO_TRANSLITERATION "
                + "-DUCONFIG_NO_REGULAR_EXPRESSIONS "
                + "-DUCONFIG_NO_BREAK_ITERATION "
                + "-DU_CHARSET_IS_UTF8"
            )
        }
    )

    # Exclude data that we currently don't need.
    #
    # The file format for ICU's data build tool is described at
    # <https://github.com/unicode-org/icu/blob/master/docs/userguide/icu_data/buildtool.md>.
    env["ICU_DATA_FILTER_FILE"] = mozpath.join(topsrcdir, "intl/icu/data_filter.json")

    print("Running ICU configure...")
    if not try_run(
        "icu-configure",
        [
            "sh",
            configure,
            "--with-data-packaging=archive",
            "--enable-static",
            "--disable-shared",
            "--disable-extras",
            "--disable-icuio",
            "--disable-layout",
            "--disable-layoutex",
            "--disable-tests",
            "--disable-samples",
            "--disable-strict",
        ],
        cwd=objdir,
        env=env,
    ):
        return False
    print("Running ICU make...")
    if not try_run(
        "icu-make",
        ["make", "--jobs=%d" % multiprocessing.cpu_count(), "--output-sync"],
        cwd=objdir,
    ):
        return False
    print("Copying ICU data file...")
    tree_data_path = mozpath.join(topsrcdir, "config/external/icu/data/")
    old_data_file = get_data_file(tree_data_path)
    if not old_data_file:
        print("Error: no ICU data file in %s" % tree_data_path, file=sys.stderr)
        return False
    new_data_file = get_data_file(mozpath.join(objdir, "data/out"))
    if not new_data_file:
        print("Error: no ICU data in ICU objdir", file=sys.stderr)
        return False
    if os.path.basename(old_data_file) != os.path.basename(new_data_file):
        # Data file name has the major version number embedded.
        os.unlink(old_data_file)
    shutil.copy(new_data_file, tree_data_path)
    try:
        shutil.rmtree(objdir)
    except Exception:
        print("Warning: failed to remove %s" % objdir, file=sys.stderr)
    return True


def main():
    if len(sys.argv) != 2:
        print("Usage: icu_sources_data.py <mozilla topsrcdir>", file=sys.stderr)
        sys.exit(1)

    topsrcdir = mozpath.abspath(sys.argv[1])
    update_sources(topsrcdir)
    if not update_data_file(topsrcdir):
        print("Error updating ICU data file", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
