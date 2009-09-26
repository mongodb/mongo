import os
import urllib
import urllib2
try:
    import json
except:
    import simplejson as json # need simplejson for python < 2.6

import settings

def machine_info(extra_info=""):
    """Get a dict representing the "machine" section of a benchmark result.

    ie:
    {
        "os_name": "OS X",
        "os_version": "10.5",
        "processor": "2.4 GHz Intel Core 2 Duo",
        "memory": "3 GB 667 MHz DDR2 SDRAM",
        "extra_info": "Python 2.6"
    }

    Must have a settings.py file on sys.path that defines "processor" and "memory"
    variables.
    """
    machine = {}
    (machine["os_name"], _, machine["os_version"], _, _) = os.uname()
    machine["processor"] = settings.processor
    machine["memory"] = settings.memory
    machine["extra_info"] = extra_info
    return machine

def post_data(data, machine_extra_info="", post_url="http://mongo-db.appspot.com/benchmark"):
    """Post a benchmark data point.

    data should be a Python dict that looks like:
        {
          "benchmark": {
            "project": "http://github.com/mongodb/mongo-python-driver",
            "name": "insert test",
            "description": "test inserting 10000 documents with the C extension enabled",
            "tags": ["insert", "python"]
          },
          "trial": {
            "server_hash": "4f5a8d52f47507a70b6c625dfb5dbfc87ba5656a",
            "client_hash": "8bf2ad3d397cbde745fd92ad41c5b13976fac2b5",
            "result": 67.5,
            "extra_info": "some logs or something"
          }
        }
    """
    data["machine"] = machine_info(machine_extra_info)
    urllib2.urlopen(post_url, urllib.urlencode({"payload": json.dumps(data)}))
    return data
