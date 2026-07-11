import json
import sys


def get_mongot_extension_name(json_path):
    with open(json_path, "r") as f:
        return json.load(f)["name"]


if __name__ == "__main__":
    output = sys.argv[1]
    json_path = sys.argv[2]
    name = get_mongot_extension_name(json_path)
    with open(output, "w") as f:
        f.write(f"""// GENERATED FILE. DO NOT EDIT.
// Generated from {json_path}. The mongot extension's canonical name lives there so the
// server (this file's consumer) and resmoke's extension config generator stay in sync on
// what value is expected on the '--loadExtensions' command line.
#pragma once

#include <string_view>

namespace mongo::search_helper_bson_obj::detail {{

constexpr std::string_view kMongotExtensionName = "{name}";

}}  // namespace mongo::search_helper_bson_obj::detail
""")
