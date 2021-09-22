set -euo pipefail

if [ -d jepsen ]; then
  echo "Cleanup docker containers"
  # docker ps -q fails when no containers are running
  sudo docker container kill $(docker ps -q) || true
  sudo docker system prune -f
fi
