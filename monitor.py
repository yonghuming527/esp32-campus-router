"""Read USB serial without requesting a reset; preserve an evidence log."""
import argparse
import pathlib
import sys
import time
import serial

parser = argparse.ArgumentParser()
parser.add_argument('--port', required=True)
parser.add_argument('--seconds', type=float, default=45)
parser.add_argument('--output', default='serial.log')
args = parser.parse_args()
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
port = serial.Serial()
port.port = args.port
port.baudrate = 115200
port.timeout = 0.2
port.dtr = True
port.rts = False
port.open()
deadline = time.monotonic() + args.seconds
with port, pathlib.Path(args.output).open('ab') as log:
    while time.monotonic() < deadline:
        data = port.read(port.in_waiting or 1)
        if data:
            log.write(data)
            log.flush()
            print(data.decode('utf-8', errors='replace'), end='', flush=True)
