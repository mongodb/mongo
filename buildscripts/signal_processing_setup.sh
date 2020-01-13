#!/usr/bin/env bash
rm -rf ./signal_processing_venv
rm -f ./analysis.yml
rm -f ./.evergreen.yml

# Configure signal processing
cat > ./analysis.yml << EOF
mongo_uri: mongodb+srv://${analysis_user}:${analysis_password}@performancedata-g6tsc.mongodb.net/perf
is_patch: ${is_patch}
task_id: ${task_id}
EOF

# Create the Evergreen API credentials
cat > .evergreen.yml <<END_OF_CREDS
api_server_host: https://evergreen.mongodb.com/api
api_key: "${evergreen_api_key}"
user: "${evergreen_api_user}"
END_OF_CREDS

# need to create virtualenv here to configure it below
virtualenv -p /opt/mongodbtoolchain/v3/bin/python3 signal_processing_venv

# Setup pip to use our internal PyPI
cat > ./signal_processing_venv/pip.conf << EOF
[global]
index-url = https://pypi.org/simple
extra-index-url = https://${perf_jira_user}:${perf_jira_pw}@artifactory.corp.mongodb.com/artifactory/api/pypi/mongodb-dag-local/simple
EOF

source ./signal_processing_venv/bin/activate
pip install dag-signal-processing~=1.0.14
