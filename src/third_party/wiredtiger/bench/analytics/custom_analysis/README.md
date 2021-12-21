# Custom analysis of wiredtiger performance data

This folder contains `Python notebook` examples with custom analysis of wiredtiger performance data, either via [Evergreen](evergreen_analysis.ipynb) or via [Atlas](atlas_analysis.ipynb).

### How to run
To run these notebooks perform the following steps:
```
virtualenv -p python3 venv
source venv/bin/activate
pip3 install jupyter requests matplotlib pymongo==3.12.2
jupyter notebook
```
and open the return URL
