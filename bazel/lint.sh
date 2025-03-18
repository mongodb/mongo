# placeholder for bazel/wrapper_hook/lint.py
if [[ $1 == "ALL_PASSING" ]]; then
  echo "No linter errors found!"
  exit 0
fi

echo "Linter run failed, see details above"
exit 1
