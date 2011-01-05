import re

def parse_config(config):
    for l in re.findall(r'(?:\(.*?\)|[^,])+', config):
        for k, v in (l+"=yes").split('=')[0:2]:
            yield (k.strip(), v.strip())
