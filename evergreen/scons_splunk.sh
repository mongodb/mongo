DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

curator splunk --json --url=${scons_splunk_server} --token=${scons_splunk_token} --annotation=project:${project} --annotation=task_id:${task_id} --annotation=build_variant:${build_variant} --annotation=git_revision:${revision} command --exec="cat src/scons_cache.log.json" >splunk_stdout.txt || cat splunk_stdout.txt
