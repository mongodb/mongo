#!/usr/bin/env python3
"""Bypass compile and fetch binaries for burn_in_tags."""

from urllib.parse import urlparse
from buildscripts.bypass_compile_and_fetch_binaries import (
    find_suitable_build_id, generate_bypass_expansions, parse_args, read_evg_config,
    requests_get_json, write_out_bypass_compile_expansions)


def main():  # pylint: disable=too-many-locals,too-many-statements
    """Execute Main program."""

    args = parse_args()
    evg_config = read_evg_config()
    if evg_config is None:
        print("Could not find ~/.evergreen.yml config file. Default compile bypass to false.")
        return

    api_server = "{url.scheme}://{url.netloc}".format(
        url=urlparse(evg_config.get("api_server_host")))
    revision_url = f"{api_server}/rest/v1/projects/{args.project}/revisions/{args.revision}"
    revisions = requests_get_json(revision_url)
    build_id = find_suitable_build_id(revisions["builds"], args)
    if not build_id:
        print("Could not find build id for revision {args.revision} on project {args.project}."
              " Default compile bypass to false.")
        return

    expansions = generate_bypass_expansions(args.project, args.buildVariant, args.revision,
                                            build_id)
    write_out_bypass_compile_expansions(args.outFile, **expansions)


if __name__ == "__main__":
    main()
