import sys
from typing import Any, Dict

def parse_wiredtiger_config(config_str: str) -> Dict[str, Any]:
    """
    Recursively parse a WiredTiger configuration string into a nested dictionary.
    It handles nested parentheses and quoted values.
    """
    def parse_section(s: str, start: int = 0) -> (Dict[str, Any], int):
        """
        Parse a section of the WiredTiger config string starting from the given index.
        :param s: The WiredTiger config string to parse.
        :param start: The index to start parsing from.
        :return: A tuple containing a dictionary of parsed_result key-value pairs and the next index to continue parsing.
        """
        result = {}
        key = ''
        value = ''
        i = start
        in_key = True # Track if we are reading a key. Start with expecting a key.
        in_quotes = False # Track if we are inside quotes. Start with not in quotes.

        while i < len(s):
            c = s[i]
            if in_key:
                # If we are reading a key, we expect characters until we hit '=' or ','.
                if c == '=':
                    if not key:
                        raise ValueError("Invalid config string: key cannot be empty before '='")
                    # If hitting '=', we switch to reading value.
                    in_key = False
                    i += 1
                    continue
                elif c == ',' and not in_quotes:
                    # If hitting ',' and not inside quotes, we finalize the current key-value pair.
                    if key:
                        # If the key is not empty, still add it to the result.
                        result[key.strip()] = ''
                        # Reset key for the next pair.
                        key = ''
                    i += 1
                    continue
                key += c
            else:
                # If we are reading a value, we expect characters until we hit ')' or ','.
                if c == '"':
                    # Toggle in_quotes state when encountering a quote.
                    in_quotes = not in_quotes
                    value += c
                elif c == '(' and not in_quotes:
                    # Recursively parse nested section
                    nested, new_i = parse_section(s, i + 1)
                    value = nested
                    i = new_i
                    continue  # Prevent value += c after assigning dict
                elif c == ')' and not in_quotes:
                    # If hitting ')', finalize the current key-value pair and return.
                    if key:
                        result[key.strip()] = value if value != '' else ''
                    return result, i + 1
                elif c == ',' and not in_quotes:
                    # If hitting ',' and not inside quotes, finalize the current key-value pair.
                    result[key.strip()] = value if value != '' else ''
                    key = ''
                    value = ''
                    in_key = True
                else:
                    value += c
            i += 1
        if key:
            result[key.strip()] = value if value != '' else ''
        return result, i

    parsed_result, _ = parse_section(config_str)
    return parsed_result

def parse_turtle_file(filename: str) -> Dict[str, Any]:
    """
    Parse the last line of a WiredTiger.turtle file as a WiredTiger config string.
    """
    with open(filename, 'r') as f:
        lines = [line.strip() for line in f if line.strip()]
    if not lines:
        raise ValueError("File is empty or contains only blank lines.")

    # Only parse the last line which is expected to be the config string.
    last_line = lines[-1]
    return parse_wiredtiger_config(last_line)

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: python parse_wiredtiger_config.py /path/to/WiredTiger.turtle")
        sys.exit(1)
    filename = sys.argv[1]
    try:
        result = parse_turtle_file(filename)
        import pprint
        pprint.pprint(result)
    except Exception as e:
        print(f"Error: {e}")

