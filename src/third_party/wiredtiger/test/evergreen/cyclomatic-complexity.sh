#!/bin/bash

# Install Metrix++, ensuring it is outside the 'src' directory
source test/evergreen/download_metrixpp.sh

mkdir -p code_statistics_report

# We only want complexity measures for the 'src' directory
cd src

python3 "../metrixplusplus/metrix++.py" collect --std.code.lines.code --std.code.complexity.cyclomatic
python3 "../metrixplusplus/metrix++.py" view

# Set the cyclomatic complexity limit to 20
python3 "../metrixplusplus/metrix++.py" limit --max-limit=std.code.complexity:cyclomatic:20

# Fail if there are functions with cyclomatic complexity larger than 98
python "../metrixplusplus/metrix++.py" limit --max-limit=std.code.complexity:cyclomatic:98 > $t
if grep -q 'exceeds' $t; then
    echo "[ERROR]:complexity:cyclomatic: Complexity limit exceeded."
    cat $t
    echo "[ERROR]:complexity:cyclomatic: Finished " && rm $t && exit 1
else
    cat $t && rm $t
fi

python3 "../metrixplusplus/metrix++.py" view --format=python > ../code_statistics_report/code_complexity_summary.json
python3 "../metrixplusplus/metrix++.py" export --db-file=metrixpp.db > ../code_statistics_report/metrixpp.csv

# Generate the code complexity statistics that is compatible with Atlas.
virtualenv -p python3 venv
source venv/bin/activate
pip3 install pandas==2.2.2

python3 ../test/evergreen/code_complexity_analysis.py -s ../code_statistics_report/code_complexity_summary.json -d ../code_statistics_report/metrixpp.csv -o ../code_statistics_report/atlas_out_code_complexity.json
