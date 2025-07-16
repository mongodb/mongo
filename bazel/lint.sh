# placeholder for bazel/wrapper_hook/lint.py

RED='\033[0;31m'
GREEN='\033[0;32m'
NO_COLOR='\033[0m'

if [[ $1 == "ALL_PASSING" ]]; then
    echo -e "${GREEN}INFO:${NO_COLOR} No linter errors found!"
    exit 0
fi

echo -e "${RED}ERROR:${NO_COLOR} Linter run failed, see details above"
echo -e "${GREEN}INFO:${NO_COLOR} Run the following to try to auto-fix the errors:\n\nbazel run lint --fix"

exit 1
