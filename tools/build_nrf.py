#!/usr/bin/env python3
"""Build both DK cores using an existing nRF Connect SDK 3.0.1 installation."""
import argparse
import os
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[1]
COMMITS = {
    'nrf': '9eb5615da66b01cd9265b02aabe095f47c98baa3',
    'zephyr': '77f865b8f8d0cb3d19002bfe713e9dd46e6f71b7',
}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ncs', type=Path, required=True, help='NCS workspace containing nrf and zephyr')
    args = parser.parse_args()
    ncs = args.ncs.resolve()
    for project, expected in COMMITS.items():
        actual = subprocess.check_output(['git', '-C', str(ncs / project), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != expected:
            parser.error(project + ' must be the pinned NCS 3.0.1 revision ' + expected)
    env = dict(os.environ, ZEPHYR_BASE=str(ncs / 'zephyr'))
    subprocess.run(['west', 'build', '-b', 'nrf5340dk/nrf5340/cpuapp', '--sysbuild',
                    '-d', str(ROOT / 'build/bridge-nrf'), str(ROOT / 'firmware/nrf')],
                   cwd=ncs, env=env, check=True)

if __name__ == '__main__':
    main()
