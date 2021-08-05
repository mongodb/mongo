DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

client_logs=$(ls crud*.log fsm*.log 2> /dev/null)
if [ ! -z "$client_logs" ]; then
  ${tar} czf client-logs.tgz $client_logs
fi
