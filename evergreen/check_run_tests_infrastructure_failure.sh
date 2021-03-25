set -o verbose
cd src
if [ -f run_tests_infrastructure_failure ]; then
  exit $(cat run_tests_infrastructure_failure)
fi
