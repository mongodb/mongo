set -o errexit
set -o verbose

cd antithesis/topologies/sharded_cluster
sudo docker-compose up -d
sudo docker exec workload /bin/bash -c 'cd resmoke && . python3-venv/bin/activate && python3 run_suite.py'
