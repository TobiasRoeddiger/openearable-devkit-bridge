#!/usr/bin/env python3
"""Read firmware diagnostics over an explicitly selected J-Link; never flash/reset."""
import argparse,re,subprocess,time,shutil
from pathlib import Path
import pylink
p=argparse.ArgumentParser();p.add_argument('--probe',type=int,required=True);p.add_argument('--elf',type=Path)
p.add_argument('--nm',default='arm-zephyr-eabi-nm',help='nm executable from the NCS toolchain, required with --elf')
p.add_argument('--pace',type=float,default=0,help='Seconds between command bytes for small shell RX buffers')
p.add_argument('--speed',type=int,default=1000,help='SWD clock in kHz')
p.add_argument('--newline',choices=['lf','cr'],default='lf',help='Use cr for the earable Zephyr shell')
p.add_argument('--command',action='append',default=[]);p.add_argument('--seconds',type=float,default=10);p.add_argument('--output',type=Path)
a=p.parse_args();address=None
if a.elf:
    nm=shutil.which(a.nm)
    if not nm: p.error('--nm must identify arm-zephyr-eabi-nm from the NCS toolchain')
    symbols=subprocess.check_output([nm,str(a.elf)],text=True)
    match=re.search(r'^([0-9a-f]+) B _SEGGER_RTT$',symbols,re.M)
    if not match:raise SystemExit('No RTT control block in supplied ELF')
    address=int(match[1],16)
probe=pylink.JLink();probe.open(serial_no=a.probe);print('Target mV:',probe.hardware_status.VTarget,flush=True)
probe.set_tif(pylink.enums.JLinkInterfaces.SWD);probe.connect('NRF5340_XXAA_APP',speed=a.speed)
print('Target halted:',probe.halted(),flush=True)
probe.rtt_start(block_address=address) if address else probe.rtt_start()
result=[]
def drain():
    text=bytes(probe.rtt_read(0,8192)).decode(errors='replace')
    if text:print(text,end='',flush=True);result.append(text)
try:
    time.sleep(.3)
    drain()
    for command in a.command:
        encoded=(command+('\r' if a.newline=='cr' else '\n')).encode()
        offset=0;write_deadline=time.monotonic()+10
        while offset<len(encoded):
            if time.monotonic()>write_deadline:
                raise TimeoutError('RTT command buffer is full; command was not delivered')
            chunk=encoded[offset:offset+1] if a.pace else encoded[offset:]
            written=probe.rtt_write(0,chunk)
            offset+=written
            drain()
            time.sleep(a.pace if a.pace else .01)
    deadline=time.monotonic()+a.seconds
    while time.monotonic()<deadline:
        drain()
        time.sleep(.05)
finally:
    probe.rtt_stop();probe.close()
    if a.output:a.output.write_text(''.join(result))
