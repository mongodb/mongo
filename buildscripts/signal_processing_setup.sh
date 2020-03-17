#!/usr/bin/env bash
if [[ -d "signal_processing_venv" ]]; then
    source ./signal_processing_venv/bin/activate
    exit 0
fi

# Configure signal processing
cat > .signal-processing.yml << EOF
task_id: ${task_id}
is_patch: ${is_patch}
mongo_uri: mongodb+srv://${dsi_analysis_atlas_user}:${dsi_analysis_atlas_pw}@performancedata-g6tsc.mongodb.net/perf
evergreen:
  api_key: "${evergreen_api_key}"
  user: "${evergreen_api_user}"
  api_server_host: https://evergreen.mongodb.com
EOF
virtualenv -p /opt/mongodbtoolchain/v3/bin/python3 signal_processing_venv

# Setup pip to use our internal PyPI
cat > ./signal_processing_venv/pip.conf << EOF
[global]
index-url = https://pypi.org/simple
extra-index-url = https://${perf_jira_user}:${perf_jira_pw}@artifactory.corp.mongodb.com/artifactory/api/pypi/mongodb-dag-local/simple
EOF

source ./signal_processing_venv/bin/activate
pip install dag-signal-processing~=2.0.0

export analysis_user="${dsi_analysis_atlas_user}"
export analysis_password="${dsi_analysis_atlas_pw}"