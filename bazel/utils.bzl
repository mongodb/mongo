# General starlark utility functions

def retry_download_and_extract(ctx, tries, **kwargs):
    sleep_time = 1
    for attempt in range(tries):
        is_retriable = attempt + 1 < tries
        result = ctx.download_and_extract(allow_fail = is_retriable, **kwargs)
        if result.success:
            return result
        else:
            print("Download failed (Attempt #%s), sleeping for %s seconds then retrying..." % (attempt + 1, sleep_time))
            ctx.execute(["sleep", str(sleep_time)])
            sleep_time *= 2

def get_host_distro_major_version(repository_ctx):
    distro_pattern_map = {
        "Ubuntu 22*": "ubuntu22",
        "Amazon Linux 2": "amazon_linux_2",
        "Amazon Linux 2023": "amazon_linux_2023",
        "Red Hat Enterprise Linux 8*": "rhel8",
    }

    if repository_ctx.os.name != "linux":
        return None

    result = repository_ctx.execute([
        "sed",
        "-n",
        "/^\\(NAME\\|VERSION_ID\\)=/{s/[^=]*=//;s/\"//g;p}",
        "/etc/os-release",
    ])

    if result.return_code != 0:
        print("Failed to determine system distro, parsing os-release failed with the error: " + result.stderr)
        return None

    distro_seq = result.stdout.splitlines()
    if len(distro_seq) != 2:
        print("Failed to determine system distro, parsing os-release returned: " + result.stdout)
        return None

    distro_str = "{distro_name} {distro_version}".format(
        distro_name = distro_seq[0],
        distro_version = distro_seq[1],
    )

    for distro_pattern, simplified_name in distro_pattern_map.items():
        if "*" in distro_pattern:
            prefix_suffix = distro_pattern.split("*")
            if distro_str.startswith(prefix_suffix[0]) and distro_str.endswith(prefix_suffix[1]):
                return simplified_name
        elif distro_str == distro_pattern:
            return simplified_name
    return None
