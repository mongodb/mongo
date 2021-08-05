cd src
# Find all core files and move to src
core_files=$(/usr/bin/find -H .. \( -name "*.core" -o -name "*.mdmp" \) 2> /dev/null)
for core_file in $core_files; do
  base_name=$(echo $core_file | sed "s/.*\///")
  # Move file if it does not already exist
  if [ ! -f $base_name ]; then
    mv $core_file .
  fi
done
