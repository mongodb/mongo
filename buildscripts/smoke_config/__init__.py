
import os

USER_CONFIG_DIRS = (os.path.split(__file__)[0], "~/.smoke_config")


def get_named_configs(search_paths=USER_CONFIG_DIRS):
    """Extract named JSON configurations from specified paths."""
    named_configs = {}
    for search_path in search_paths:

        search_path = os.path.expanduser(search_path)
        if not os.path.isdir(search_path):
            continue

        for path, dirnames, filenames in os.walk(search_path):

            for filename in filenames:

                filebase, ext = os.path.splitext(filename)
                if ext != ".json" and ext != ".yaml" and ext != ".yml":
                    continue

                named_configs[filebase] = os.path.abspath(os.path.join(path, filename))

    return named_configs
