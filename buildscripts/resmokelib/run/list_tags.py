"""Functions to parse suite yaml files to get tags and comments around them."""

import re
import textwrap


def parse_tags_blocks(suite):
    """Get substrings from the suite string where tags are defined."""
    yaml_tag_keys = ["include_with_any_tags", "exclude_with_any_tags"]
    yaml_tag_keys_regex = "|".join(yaml_tag_keys)
    all_tags_regex = re.compile(rf"(({yaml_tag_keys_regex}):\n(\s*(-|#)\s*.*)*)")
    return [tag_block[0] for tag_block in all_tags_regex.findall(suite)]


def get_tags_blocks(suite_file):
    """Get substrings from the suite file where tags are defined."""
    tags_blocks = []
    try:
        with open(suite_file, "r") as fh:
            tags_blocks = parse_tags_blocks(fh.read())
    except FileNotFoundError:
        pass
    return tags_blocks


def split_into_tags(tags_block):
    """Split tag block into lines representing each tag."""
    tags_block_lines = tags_block.split("\n")[1:]
    splitted_tags_block = []
    i = 0
    for line in tags_block_lines:
        if len(splitted_tags_block) <= i:
            splitted_tags_block.append([])
        line = line.strip()
        splitted_tags_block[i].append(line)
        if line.startswith("-"):
            i += 1
    return splitted_tags_block


def get_tag_doc(single_tag_block):
    """Get tag name with its documentation string."""
    tag_name = ""
    doc = ""
    for line in single_tag_block:
        if line.startswith("#"):
            doc += re.sub(r"^#+\s*", "", line).strip() + "\n"
        elif line.startswith("-"):
            if "#" in line:
                tag_name, comment = line.split("#", 1)
                tag_name = tag_name.replace("- ", "").strip()
                doc += comment.strip()
            else:
                tag_name = line.replace("- ", "").strip()
            doc = doc.strip()
    return tag_name, doc


def make_output(tag_docs):
    """Make output string."""
    output = ""
    for tag, doc in sorted(tag_docs.items()):
        newline = "\n"
        wrapped_doc = textwrap.indent(doc, "\t")
        output = f"{output}{newline}{tag}:{newline}{wrapped_doc}"
    return output
