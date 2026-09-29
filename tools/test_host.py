#!/usr/bin/env python3
"""Run identity/integrity and microphone gain/limiter checks on the host."""
import os
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[1]
out = ROOT / 'build/host-tests'
out.mkdir(parents=True, exist_ok=True)
for compiler, source, target, language in [
    (os.environ.get('CC', 'cc'), 'test_identity.c', 'identity', '-std=c11'),
    (os.environ.get('CXX', 'c++'), 'test_microphone_level.cpp', 'microphone', '-std=c++17'),
]:
    binary = out / target
    subprocess.run([compiler, language, '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I' + str(ROOT / 'common'),
                    str(ROOT / 'tests' / source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
