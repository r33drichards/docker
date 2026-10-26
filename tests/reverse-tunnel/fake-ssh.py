#!/usr/bin/python3
"""Network-free SSH stand-in. Records only synthetic test arguments."""
import json
import os
import sys
import time
with open('/tmp/ssh-events', 'a') as out:
    out.write(json.dumps({'pid': os.getpid(), 'argv': sys.argv[1:], 'time': time.monotonic()}) + '\n')
if os.path.exists('/tmp/ssh-fail'):
    sys.exit(1)
while True:
    time.sleep(1)
