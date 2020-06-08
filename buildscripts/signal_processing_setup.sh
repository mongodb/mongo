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

source ./signal_processing_venv/bin/activate
pip install --upgrade pip
pip install ../src/signal_processing/signal-processing
