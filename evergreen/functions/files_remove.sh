DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

if [ -z "${files}" ]; then
  exit 0
fi
for file in ${files}; do
  if [ -f "$file" ]; then
    echo "Removing file $file"
    rm -f $file
  fi
done
