#!/bin/sh
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

set -e

# Update to an ICU release:
#   Usage: update-icu.sh <URL of ICU GIT> <release tag name>
#   E.g., for ICU 62.1: update-icu.sh https://github.com/unicode-org/icu.git release-62-1
#
# Update to an ICU maintenance branch:
#   Usage: update-icu.sh <URL of ICU GIT> <maintenance name>
#   E.g., for ICU 62.1: update-icu.sh https://github.com/unicode-org/icu.git maint/maint-62

if [ $# -lt 2 ]; then
  echo "Usage: update-icu.sh <URL of ICU GIT> <release tag name>"
  exit 1
fi

# Ensure that $Date$ in the checked-out git files expands timezone-agnostically,
# so that this script's behavior is consistent when run from any time zone.
export TZ=UTC

# Also ensure GIT-INFO is consistently English.
export LANG=en_US.UTF-8
export LANGUAGE=en_US
export LC_ALL=en_US.UTF-8

icu_dir=`dirname $0`/icu

# Remove intl/icu/source, then replace it with a clean export.
rm -rf ${icu_dir}/source
tmpclonedir=$(mktemp -d)
git clone --depth 1 --branch $2 $1 ${tmpclonedir}
cp -r ${tmpclonedir}/icu4c/source ${icu_dir}/source

# Record `git log`.
# (This ensures that if ICU modifications are performed properly, it's always
# possible to run the command at the top of this script and make no changes to
# the tree.)
git -C ${tmpclonedir} log -1 > ${icu_dir}/GIT-INFO

# Clean up after ourselves.
rm -rf ${tmpclonedir}

# Remove layoutex, tests, and samples, but leave makefiles and test data in place.
find ${icu_dir}/source/layoutex -name '*Makefile.in' -prune -or -type f -print | xargs rm
find ${icu_dir}/source/test -name '*Makefile.in' -prune -or -name 'testdata' -prune -or -type f -print | xargs rm
find ${icu_dir}/source/samples -name '*Makefile.in' -prune -or -type f -print | xargs rm

for patch in \
 bug-915735 \
 suppress-warnings.diff \
 bug-1198952-workaround-make-3.82-bug.diff \
 bug-1614941-dsb-hsb-dates.diff \
 bug-1636984-alias-append-items-sink.diff \
 bug-1636984-display-name-fractional-seconds.diff \
 bug-1636984-append-item-dayperiod-fractional-seconds.diff \
 bug-1706949-wasi-workaround.diff \
 bug-1714933-1-locale-unicode-keywords.diff \
 bug-1714933-2-locale-basename-memory-leak.diff \
 bug-1714933-3-locale-nullptr-deref.diff \
; do
  echo "Applying local patch $patch"
  patch -d ${icu_dir}/../../ -p1 --no-backup-if-mismatch < ${icu_dir}/../icu-patches/$patch
done

topsrcdir=`dirname $0`/../
python ${topsrcdir}/js/src/tests/non262/String/make-normalize-generateddata-input.py $topsrcdir

# Update our moz.build files in config/external/icu, and build a new ICU data
# file.
python `dirname $0`/icu_sources_data.py $topsrcdir

hg addremove "${icu_dir}/source" "${icu_dir}/GIT-INFO" ${topsrcdir}/config/external/icu

# Check local tzdata version.
`dirname $0`/update-tzdata.sh -c

# CLDR updates may lead to new language tag mappings, so we need to call make_intl_data.py, too.
echo "INFO: Please run 'js/src/builtin/intl/make_intl_data.py langtags' to update additional language tag files for SpiderMonkey."
