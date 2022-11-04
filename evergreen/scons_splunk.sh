DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

touch src/scons_cache.log.json
# TODO EVG-18207 remove GODEBUG=asyncpreemptoff=1
GODEBUG=asyncpreemptoff=1 curator --level warning splunk --json --url=${scons_splunk_server} --token=${scons_splunk_token} --annotation=project:${project} --annotation=task_id:${task_id} --annotation=build_variant:${build_variant} --annotation=git_revision:${revision} follow --file src/scons_cache.log.json
