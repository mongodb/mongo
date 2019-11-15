#!/usr/bin/env bash
if [[ -d "signal_processing_venv" ]]; then
    source ./signal_processing_venv/bin/activate
    exit 0
fi

# Configure signal processing
cat > ./analysis.yml << EOF
mongo_uri: mongodb+srv://\${analysis_user}:\${analysis_password}@performancedata-g6tsc.mongodb.net/perf
is_patch: ${is_patch}
task_id: ${task_id}
EOF
virtualenv -p /opt/mongodbtoolchain/v3/bin/python3 signal_processing_venv

# Setup pip to use our internal PyPI
cat > ./signal_processing_venv/pip.conf << EOF
[global]
index-url = https://pypi.org/simple
extra-index-url = https://${perf_jira_user}:${perf_jira_pw}@artifactory.corp.mongodb.com/artifactory/api/pypi/mongodb-dag-local/simple
EOF

# Create the Evergreen API credentials
cat > .evergreen.yml <<END_OF_CREDS
api_server_host: https://evergreen.mongodb.com/api
api_key: "${evergreen_api_key}"
user: "${evergreen_api_user}"
END_OF_CREDS

source ./signal_processing_venv/bin/activate
pip install dag-signal-processing~=1.0.14

export analysis_user="${dsi_analysis_atlas_user}"
export analysis_password="${dsi_analysis_atlas_pw}"