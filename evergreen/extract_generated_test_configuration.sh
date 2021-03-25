set -o verbose
set -o errexit

target_dir="src/generated_resmoke_config"
mkdir -p $target_dir
mv generate_tasks_config.tgz $target_dir

cd $target_dir
tar xzf generate_tasks_config.tgz
