from typing import Any

# PyYaml is very easy to use, but it is very slow. This is a problem for us since the main evergreen.yml file is quite large.
# PyYaml was taking over 10s to just load the file, which needed to be done every single task and so was a significant bottleneck.
# We use the rapidyaml library instead, which is much more low level but much faster (sub 1s to load the same file). This is not a
# full drop in replacement for PyYaml and does not fully satisfy the yaml spec, but it is sufficient for our needs.


try:
    import ryml

    def ryml_to_dict(tree: ryml.Tree, index: int = 0) -> Any:
        """Walk through the ryml tree and convert nodes."""
        if tree.is_map(index):
            return {
                str(tree.key(child_index), "utf8"): ryml_to_dict(tree, child_index)
                for child_index in ryml.children(tree, index)
            }
        elif tree.is_seq(index):
            return [ryml_to_dict(tree, child_index) for child_index in ryml.children(tree, index)]
        else:
            decoded_value = tree.val(index).tobytes().decode("utf8")
            if decoded_value == "true":
                return True
            elif decoded_value == "false":
                return False
            elif decoded_value == "null" or decoded_value == "~":
                return None
            try:
                int_value = int(decoded_value)
                return int_value
            except ValueError:
                pass
            try:
                float_value = float(decoded_value)
                return float_value
            except ValueError:
                pass
            return decoded_value

    def yaml_load(data: str) -> dict:
        """Safely load YAML data."""
        return ryml_to_dict(ryml.parse_in_arena(data))

except ImportError:
    from yaml import safe_load as yaml_load  # noqa
