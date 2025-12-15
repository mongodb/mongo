import sys

import yaml


def get_extension_confs(file_path, extension_name):
    with open(file_path, "r") as file:
        data = yaml.safe_load(file)
    return data["extensions"][extension_name]


if __name__ == "__main__":
    output = sys.argv[1]
    conf_file = sys.argv[2]
    extension_name = sys.argv[3]
    conf_data = get_extension_confs(conf_file, extension_name)
    new_conf = dict(conf_data)
    new_conf["sharedLibraryPath"] = f"lib/lib{extension_name}_mongo_extension.so"
    with open(output, "w") as stream:
        yaml.dump(new_conf, stream)
