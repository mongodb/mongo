#!/bin/bash

. `dirname -- ${BASH_SOURCE[0]}`/common_functions.sh
cd_top
set -ueo pipefail
cd dist/modularity
. ./init.sh
./scan_sources.py $TOP_DIR "$@"
