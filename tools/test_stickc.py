#!/usr/bin/env python3
"""Run native C++ tests on Windows (clang++) or Unix (c++)."""
from pathlib import Path
import shutil
import subprocess
import os

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'test/build'


def run(args):
    subprocess.run([str(v) for v in args], cwd=ROOT, check=True)


if __name__ == '__main__':
    OUT.mkdir(parents=True, exist_ok=True)
    cxx = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++') or shutil.which('c++')
    if not cxx:
        raise SystemExit('Install a host C++ compiler (clang++ / g++) first.')
    sources = ['main.cpp', 'shim/shim.cpp', 'test_pet.cpp', 'test_dex.cpp',
               'test_i18n.cpp', 'test_blit.cpp', 'test_batcurve.cpp', 'test_power.cpp',
               'test_bootbutton.cpp', 'test_screenlock.cpp',
               'test_halfscale.cpp']
    run([cxx, '-std=c++14', '-O1', '-Itest/shim', '-o', OUT/'upstream.exe',
         *[ROOT/'test'/s for s in sources], ROOT/'pet.cpp', ROOT/'i18n.cpp',
         ROOT/'screenlock.cpp'])
    run([OUT/'upstream.exe'])
    run([cxx, '-std=c++14', '-O1', '-o', OUT/'stickc-format.exe',
         ROOT/'test/test_stickc_format.cpp'])
    run([OUT/'stickc-format.exe', ROOT/'ports/stickc-plus2/data/monsters.pack'])
    run([cxx, '-std=c++14', '-O1', '-Itest/stickc/shim', '-Itest/shim',
         '-I.pio/libdeps/stickc-plus2/M5GFX/src', '-o', OUT/'stickc-ui.exe',
         ROOT/'test/stickc/test_ui.cpp', ROOT/'test/shim/shim.cpp', ROOT/'pet.cpp', ROOT/'i18n.cpp'])
    run([OUT/'stickc-ui.exe'])
