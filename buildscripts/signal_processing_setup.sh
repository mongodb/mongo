#!/usr/bin/env bash
rm -rf ./signal_processing_venv
rm -f ./.signal-processing.yml

# Configure signal processing
cat > .signal-processing.yml << EOF
task_id: ${task_id}
is_patch: ${is_patch}
mongo_uri: mongodb+srv://${analysis_user}:${analysis_password}@performancedata-g6tsc.mongodb.net/perf
evergreen:
  api_key: "${evergreen_api_key}"
  user: "${evergreen_api_user}"
  api_server_host: https://evergreen.mongodb.com
EOF

# need to create virtualenv here to configure it below
virtualenv -p /opt/mongodbtoolchain/v3/bin/python3 signal_processing_venv

# Setup pip to use our internal PyPI
cat > ./signal_processing_venv/pip.conf << EOF
[global]
index-url = https://pypi.org/simple
extra-index-url = https://${perf_jira_user}:${perf_jira_pw}@artifactory.corp.mongodb.com/artifactory/api/pypi/mongodb-dag-local/simple
EOF

source ./signal_processing_venv/bin/activate
pip install dag-signal-processing~=2.0.0
