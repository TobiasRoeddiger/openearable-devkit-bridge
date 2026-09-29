#!/usr/bin/env python3
"""Build/flash the AAC-first bridge with the pinned ESP-IDF 6.1 checkout."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SDK_COMMIT = 'fff9895c82d744c7237be8847347bdd1b07c6643'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=ROOT / 'build/esp-idf-source')
    parser.add_argument('--tools', type=Path, default=ROOT / 'build/idf-tools')
    parser.add_argument('--flash', action='store_true')
    parser.add_argument('--port', help='Explicit ESP32 USB serial port; required with --flash')
    args = parser.parse_args()
    if args.flash and not args.port:
        parser.error('--flash requires --port; choose the ESP32 port explicitly')
    sdk = args.sdk.resolve()
    commit = subprocess.check_output(['git', '-C', str(sdk), 'rev-parse', 'HEAD'], text=True).strip()
    if commit != SDK_COMMIT:
        parser.error('This project requires ESP-IDF v6.1 at ' + SDK_COMMIT)
    env = dict(os.environ, IDF_PATH=str(sdk), IDF_TOOLS_PATH=str(args.tools.resolve()))
    python_envs = sorted((args.tools / 'python_env').glob('idf6.1_py*_env'))
    if python_envs:
        env['IDF_PYTHON_ENV_PATH'] = str(python_envs[-1].resolve())
        env['PATH'] = str(python_envs[-1].resolve() / 'bin') + os.pathsep + env['PATH']
    command = ['idf.py', '-C', str(ROOT / 'firmware/esp32-idf'), '-B',
               str(ROOT / 'build/bridge-esp32-aac')]
    if args.flash:
        command += ['-p', args.port]
    command += ['build'] + (['flash'] if args.flash else [])
    script = 'source ' + shlex.quote(str(sdk / 'export.sh')) + '\nexec ' + shlex.join(command)
    subprocess.run(['/bin/bash', '-c', script], env=env, check=True)


if __name__ == '__main__':
    main()
