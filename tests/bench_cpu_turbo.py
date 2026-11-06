#!/usr/bin/env python3
"""Headless GIOS/proj7: interleaved medians, not a trace-equivalence test.

fast gets zero DDR and instruction-clock PF to match turbo's fixed machine.
Reported intervals are guest run time, excluding image load and final shutdown.
"""
import argparse
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
IMAGES = ('gios/gios.inst.bin', 'bench/bench.inst.bin',
          'proj_app7/isr7.bin', 'proj_app7/proj7.inst.bin')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sw-dir', type=Path, default=Path(os.environ.get('CS150_SW', '/workspace/software')))
    ap.add_argument('--insns', type=int, default=200_000_000)
    ap.add_argument('--runs', type=int, default=5)
    ap.add_argument('--json', type=Path)
    args = ap.parse_args()
    if args.runs < 1 or args.insns < 1:
        ap.error('--runs and --insns must be positive')
    if not all((args.sw_dir / i).is_file() for i in IMAGES):
        print(f'SKIP: optional GIOS/proj7 binary images missing at {args.sw_dir}')
        return 0
    env = dict(os.environ)
    env.pop('VVIZARD_QUIRKS', None)
    rates = {'turbo': [], 'fast': []}
    for i in range(args.runs):
        # Reverse pair order every run to reduce startup/temperature bias.
        for cpu in (('turbo', 'fast') if i % 2 == 0 else ('fast', 'turbo')):
            cmd = [str(ROOT / 'vvizard'), f'--cpu={cpu}', '--no-window', '--sw-dir', str(args.sw_dir),
                   '--max-insns', str(args.insns)]
            if cpu == 'fast':
                cmd += ['--no-random-ddr', '--frame-clock=emulated']
            r = subprocess.run(cmd, capture_output=True, stdin=subprocess.DEVNULL, env=env, timeout=120)
            text = r.stderr.decode(errors='replace')
            if r.returncode or b'PROJ-7' not in r.stdout:
                print(f'FAIL {cpu}: rc={r.returncode}\n{text}', file=sys.stderr)
                return 1
            pattern = r': ([\d.]+) Mcyc/s' if cpu == 'turbo' else r'; ([\d.]+)M insns/s'
            m = re.search(pattern, text)
            if not m:
                print(f'FAIL {cpu}: missing rate\n{text}', file=sys.stderr)
                return 1
            rate = float(m[1]); rates[cpu].append(rate)
            print(f'{cpu:5s} run {i + 1}: {rate:.3f} Mcyc/s', flush=True)
    medians = {k: statistics.median(v) for k, v in rates.items()}
    result = {'insns': args.insns, 'runs': args.runs, 'rates_mcyc_s': rates,
              'medians_mcyc_s': medians, 'ratio': medians['turbo'] / medians['fast'],
              'mode': 'headless; PF instruction-clock; zero-initialized DDR; no stalls'}
    print(json.dumps(result, indent=2))
    if args.json:
        args.json.write_text(json.dumps(result, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
