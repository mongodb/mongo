# Running wt_verify.py

## Change Directory
Navigate to the `wiredtiger/tools/wt_verify` directory.
```bash
cd wiredtiger/tools/wt_verify
```

## Activate Virtual Environment
```bash
virtualenv -p python venv
source venv/bin/activate
```

## Install Dependencies
```bash
pip install -r requirements.txt
```

## Run wt_verify.py script
Execute `wt_verify.py` with necessary parameters:

```bash
python3 wt_verify.py -d dump_pages -hd DB_DIR -f FILENAME -o OUTPUT.TXT -v
```

- `-d [option]`: Specify dump pages option.
- `-f`: Specify the file name to process.
- `-h`: Show all the available options.
- `-hd`: Set database directory.
- `-o`: Set output file name.
- `-v`: Enable visualisation (optional).
