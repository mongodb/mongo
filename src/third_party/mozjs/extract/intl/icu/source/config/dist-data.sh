#!/bin/bash
# Copyright (C) 2020 and later: Unicode, Inc. and others.

# set VERSION to the ICU version. set top_srcdir to the parent of icurc
# Note: You need to set LD_LIBRARY_PATH/etc before calling this script.
export LD_LIBRARY_PATH=./lib:${LD_LIBRARY_PATH-/lib:/usr/lib:/usr/local/lib}
export DYLD_LIBRARY_PATH=./lib:${DYLD_LIBRARY_PATH-/lib:/usr/lib:/usr/local/lib}

if [ ! -d "${top_srcdir}" ]
then
    echo >&2 "$0: please set 'top_srcdir' to the icu/icu4c/source dir"
    exit 1
fi
LICENSE=${LICENSE-${top_srcdir}/../LICENSE}

if [ ! -f "${LICENSE}" ]
then
    echo >&2 "$0: could not load license file ${LICENSE}"
    exit 1
fi

DATFILE=${DATFILE-$(ls data/out/tmp/icudt*.dat| head -1)}

if [ ! -f "${DATFILE}" ]
then
    echo >&2 "$0: could not find DATFILE ${DATFILE}"
    exit 1
fi

VERS=$(echo ${DATFILE} | tr -d a-z/.)
VERSION=${VERSION-unknown}

if [[ "${VERSION}" = "unknown" ]];
then
    VERSION=${VERS}.0
    echo "$0: VERSION not set, using ${VERSION}"
else
    if [[ "${VERS}" != $(echo ${VERSION} | cut -d. -f1) ]]
    then
        echo >&2 "$0: Warning: Expected version ${VERSION} to start with ${VERS}..."
    fi
fi

# yeah, override ENDIANS if you want a different flavor.
#ENDIANS="b l e"
ENDIANS=${ENDIANS-"b l"}
DISTY_DIR=${DISTY_DIR-./dist/}

if [ ! -x ./bin/icupkg ]
then
    echo >&2 "$0: could not find executable ./bin/icupkg"
    exit 1
fi

echo "# Packing ${DATFILE} into data zips in dist/ for version ${VERSION}"
mkdir -p ${DISTY_DIR}/tmp

for endian in $ENDIANS;
do
    base=icu4c-${VERSION}-data-bin-${endian}.zip
    filename=icudt${VERS}${endian}.dat
    if [ -f ${DISTY_DIR}/${base} ];
    then
        echo ${DISTY_DIR}/${base} exists, skipping
        continue
    fi
    rm -f ${DISTY_DIR}/tmp/${filename}
    echo ./bin/icupkg -t${endian} ${DATFILE} ${DISTY_DIR}/tmp/${filename}
    ./bin/icupkg -t${endian} ${DATFILE} ${DISTY_DIR}/tmp/${filename}
    README=icu4c-${VERSION}-data-bin-${endian}-README.md
    cat >> ${DISTY_DIR}/tmp/${README} <<EOF
# ICU Data Zip for ${VERSION}

For information on Unicode ICU, see [http://icu-project.org](http://icu-project.org)

## Contents

This .zip file contains:

- this README
- [LICENSE](./LICENSE)
- ${filename}

## How to use this file

This file contains prebuilt data in form **${endian}**.
("l" for Little Endian, "b" for Big Endian, "e" for EBCDIC.)
It may be used to simplify build and installation of ICU.
See [http://icu-project.org](http://icu-project.org) for further information.

## License

See [LICENSE](./LICENSE).

> Copyright © 2016 and later Unicode, Inc. and others. All Rights Reserved.
Unicode and the Unicode Logo are registered trademarks
of Unicode, Inc. in the U.S. and other countries.
[Terms of Use and License](http://www.unicode.org/copyright.html)

EOF
    zip -v -j ${DISTY_DIR}/${base} \
        ${LICENSE} \
        ${DISTY_DIR}/tmp/${README} \
        ${DISTY_DIR}/tmp/${filename}
    ls -lh ${DISTY_DIR}/${base}
done
