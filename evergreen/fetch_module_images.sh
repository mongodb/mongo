set -e

for dir in ./src/src/mongo/db/modules/*; do
    if test -f $dir/evergreen/fetch_images.sh; then
        bash $dir/evergreen/fetch_images.sh
    fi
done
