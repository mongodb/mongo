#! /usr/bin/env bash
#
# Cycle through a list of test/format configurations that failed previously,
# and run format test against each of those configurations to capture issues.
#

set -u

set -e
# Switch to the Git repo toplevel directory
cd $(git rev-parse --show-toplevel)
# Walk into the test/format directory
cd cmake_build/test/format
set +e

# Check the existence of 't' binary
if [ ! -x "t" ]; then
	echo "'t' binary does not exist, exiting ..."
	exit 1
fi

success=0
failure=0

# Cycle through format CONFIGs recorded under the "failure_configs" directory
for config in $(find ../../../test/format/failure_configs/ -name "CONFIG.*" | sort)
do
	echo -e "\nTesting CONFIG $config ...\n"
	if (./t -1 -c $config -h WT_TEST); then
		let "success++"
	else
		let "failure++"
		[ -f WT_TEST/CONFIG ] && cat WT_TEST/CONFIG
		mv WT_TEST WT_TEST_$(basename $config)
	fi
done

echo -e "\nSummary of '$(basename $0)': $success successful CONFIG(s), $failure failed CONFIG(s)\n"

[[ $failure -ne 0 ]] && exit 1
exit 0
