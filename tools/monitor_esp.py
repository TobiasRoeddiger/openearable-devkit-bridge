#!/usr/bin/env python3
"""Keep one ESP serial handle open across tests; append timestamped diagnostics."""
import argparse, json, os, select, sys, time
from pathlib import Path
import serial
p=argparse.ArgumentParser();p.add_argument('--port',required=True);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();s=serial.Serial();s.port=a.port;s.baudrate=115200;s.timeout=.2;s.dtr=False;s.rts=False
s.open()
pending = ''
running = True
try:
 with a.output.open('a',buffering=1) as f:
  while running:
   line=s.readline().decode(errors='replace').strip()
   if line:
    f.write(json.dumps({'time':time.time(),'line':line})+'\n')
    if line.startswith(('AUDIO_STATS','BT_STATS','CODEC ','STATUS','READY','HFP ','A2DP ','PAIR','PCM ')):
     print(line,flush=True)
   if select.select([sys.stdin],[],[],0)[0]:
    pending += os.read(sys.stdin.fileno(), 4096).decode()
    while '\n' in pending:
     command,pending = pending.split('\n',1)
     command=command.strip()
     if command=='QUIT':running=False;break
     if command:s.write((command+'\n').encode())
finally:s.close()
