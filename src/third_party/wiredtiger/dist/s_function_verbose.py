# When calling '__wt_verbose', the second parameter can only be a single verbose
# category definition i.e. can't treat the parameter as a flag/mask value.
# Iterate over uses of '__wt_verbose' and detect any invalid uses where multiple
# verbose categories are bitwise OR'd.
import re, sys

verbose_regex = re.compile('([0-9]+):\s*__wt_verbose\(.*?,(.*?)[\"\']')
bitwise_or_regex = re.compile('^.*(?<!\|)\|(?!\|).*$')
for line in sys.stdin:
    # Find all uses of __wt_verbose in a given line, capturing the line number
    # and 2nd paramter as groups.
    m = verbose_regex.findall(line)
    if len(m) != 0:
        for verb_match in m:
            if len(verb_match) != 2:
                continue
            line = verb_match[0]
            verb_parameter = verb_match[1]
            # Test if the verbose category parameter uses a bitwise OR.
            bit_m = bitwise_or_regex.search(verb_parameter)
            if bit_m != None:
                sys.stdout.write(line)
                sys.stdout.write('\n')
