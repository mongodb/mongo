set +o errexit

cd src
if [ -d "./build" ]; then
  file_list=$(find ./build -type f -name "*.gcda")
  if [ -n "$file_list" ]; then
    for gcda_file in $file_list; do
      echo "Processing file $gcda_file"
      /opt/mongodbtoolchain/v3/bin/gcov -i $gcda_file
      base_name=$(echo $gcda_file | rev | cut -f1 -d '/' | cut -f2 -d '.' | rev)
      gcov_file=$base_name.gcda.gcov
      if [ -f "$gcov_file" ]; then
        # Add a prefix to the intermediate file, since it does not have a unique name.
        # Convert the '/' to '#' in the file path.
        file_prefix=$(echo $gcda_file | sed -e 's,^\./,,' | rev | cut -f2- -d '/' | rev | tr -s '/' '#')
        new_gcov_file=$file_prefix #$base_name.gcda.gcov
        if [ ! -f $new_gcov_file ]; then
          echo "Renaming gcov intermediate file $gcov_file to $new_gcov_file"
          mv $gcov_file $new_gcov_file
        else
          # We treat this as a fatal condition and remove all of the coverage files.
          echo "Not renaming $gcov_file as $new_gcov_file already exists!"
          rm -f *.gcda.gcov
          exit 1
        fi
      fi
      rm $gcda_file
    done
  fi
fi
