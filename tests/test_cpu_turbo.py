#!/usr/bin/env python3
"""Fixed MIPS150 turbo tests. No Unicorn fidelity flags on this backend."""
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import warnings
from vvprog import prog

ROOT = Path(__file__).resolve().parents[1]
VV = os.environ.get('VVIZARD', str(ROOT / 'vvizard'))
ENV = dict(os.environ)
ENV.pop('VVIZARD_QUIRKS', None)


def run(*args):
    return subprocess.run([VV, '--cpu=turbo', '--no-window', *map(str, args)],
                          capture_output=True, stdin=subprocess.DEVNULL, env=ENV, timeout=30)


class Turbo(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        warnings.filterwarnings('ignore', category=ResourceWarning)

    def check(self, result):
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))

    def asm(self, name, *expects, extra=()):
        path, addr = prog(name)
        args = ['--no-bios', '--load', f'{path}@{addr:#x}', '--entry', hex(addr), '--max-insns', '2000000', *extra]
        for e in expects:
            args += ['--expect', e]
        self.check(run(*args))

    def words(self, words, *expects, extra=()):
        with tempfile.NamedTemporaryFile(suffix='.bin') as f:
            f.write(struct.pack('>' + 'I' * len(words), *words)); f.flush()
            args = ['--no-bios', '--load', f'{f.name}@0x60000000', '--entry', '0x60000000', '--max-insns', '100000', *extra]
            for e in expects:
                args += ['--expect', e]
            self.check(run(*args))

    def test_assembly(self):
        cases = [('loop', ['t0=30', 't2=0x11e']), ('alias', ['t3=0xbeef', 't4=0xbeef']),
                 ('hwprobe', ['s5=0', 's3=45', 's4=0']),
                 ('prng', ['s1=0xdeadbeef', 's4=1', 's5=0xab', 's6=0xab000000', 't3=0x0ab00000']),
                 ('quirk/emu_hilo', ['t2=0']), ('quirk/emu_overflow', ['t2=0x80000000']),
                 ('quirk/emu_load_use', ['t3=1', 't2=0x99']),
                 ('quirk/emu_lwlr', ['t2=0', 't0=0x50000000']),
                 ('quirk/emu_lb_sign', ['t2=0xffffff80', 't3=0xffff8001']),
                 ('quirk/emu_jalr_rd', ['t1=0x40000014', 'ra=0']),
                 ('quirk/emu_bal_untaken', ['ra=0']), ('quirk/emu_cop0', ['t2=0', 't3=0xffff']),
                 ('quirk/emu_syscall', ['t2=2']), ('quirk/emu_movz', ['t2=0']),
                 ('quirk/emu_unaligned', ['t2=0', 't4=0x11223344']),
                 ('quirk/emu_unm_load', ['t1=0', 't2=6'])]
        for name, expects in cases:
            with self.subTest(name=name):
                self.asm(name, *expects)

    def test_zero_and_load_overwrite(self):
        # lui t0,DSCR; ori t1,99; sw; lw t2; addiu t2,zero,7 (wins); zero never changes.
        self.words([0x3c085000, 0x34090099, 0xad090000, 0x8d0a0000,
                    0x240a0007, 0x24000055, 0x0000000d], 't2=7', 'zero=0')

    def test_jump_to_undecoded_zero(self):
        self.words([0x00000008, 0x34090001, 0x340a0002, 0x0000000d], 't1=1', 't2=0')

    def test_instruction_limit_at_delay_slot(self):
        for limit in range(1, 6):
            with self.subTest(limit=limit):
                self.words([0x34080001, 0x10000002, 0, 0x34080002, 0x0000000d],
                           't0=1', extra=['--max-insns', str(limit)])

    def test_timer_and_irq(self):
        # Count=0; Compare=80; IE+timer enabled; spin, with test ISR clearing/returning.
        isr, _ = prog('hw/isr')
        self.words([0x40804800, 0x34080050, 0x40885800, 0x34088001,
                    0x40886000, 0x00000000, 0x1000ffff, 0x00000000],
                   's6=1', extra=['--load', f'{isr}@0xc0000180'])

    def test_count_write_and_compare_clear(self):
        # mtc0 Count ignored registers/EPC fixed; Count write effective following cycle.
        self.words([0x34080100, 0x40884800, 0x40094800, 0x400a4800, 0x0000000d],
                   't1=0x100', 't2=0x101')

    def test_self_modifying_alias(self):
        # Build target in DDR through alias3, execute, modify it through alias1, execute again.
        self.words([0x3c083000, 0x3c09240a, 0x35290001, 0xad090000,
                    0x3c0903e0, 0x35290008, 0xad090004, 0xad000008,
                    0x0100f809, 0, 0x3c081000, 0x3c09240a, 0x35290002,
                    0xad090000, 0x0100f809, 0, 0x0000000d], 't2=2')

    def test_mmio_lanes(self):
        self.words([0x3c088000, 0x34090041, 0xa1090100, 0xa1090107, 0x0000000d],
                   extra=['--dump-lcd'])
        # The LCD path takes low dina byte: sb +0 writes NUL, sb +3 writes A.
        with tempfile.NamedTemporaryFile() as f:
            f.write(struct.pack('>7I', 0x3c088000, 0x34090041, 0xa1090100,
                                0xa1090107, 0x0000000d, 0, 0)); f.flush()
            r = run('--no-bios', '--load', f'{f.name}@0x60000000', '--entry', '0x60000000', '--dump-lcd')
            self.check(r)
            self.assertIn(b'hex 00 41 20', r.stderr)

    def test_mask_zero_store_mmio_side_read(self):
        self.words([0x3c088000, 0x340a0055, 0x8d090004, 0, 0x1120fffd, 0,
                    0xa90a000c, 0x8d0b0004, 0, 0x0000000d],
                   't2=0x55', 't3=0', extra=['--uart-in', 'A'])

    def test_flags_order_independent(self):
        bad = ['--quirks=default', '--quirks=none', '--no-quirks', '--halt-on-quirk',
               '--warn-on-quirk', '--no-halt-on-quirk', '--quiet-quirk=all', '--list-quirks',
               '--ddr-stalls', '--sync', '--threads=on', '--threads=off', '--little',
               '--no-irq', '--frame-clock=wall', '--pf-period=1', '--legacy-io', '--trace']
        for flag in bad:
            for args in ([flag, '--cpu=turbo'], ['--cpu=turbo', flag]):
                with self.subTest(args=args):
                    r = subprocess.run([VV, *args], capture_output=True, env=ENV)
                    self.assertEqual(r.returncode, 2, r.stderr)
                    self.assertIn(b'turbo does not take', r.stderr)
        e = dict(ENV, VVIZARD_QUIRKS='none')
        self.assertEqual(subprocess.run([VV, '--cpu=turbo'], env=e, capture_output=True).returncode, 2)

    def test_gpu_and_frame(self):
        p, addr = prog('hw/gp_fill_stop')
        with tempfile.NamedTemporaryFile(suffix='.ppm') as out:
            r = run('--no-bios', '--load', f'{p}@{addr:#x}', '--entry', hex(addr),
                    '--max-insns', '2000000', '--dump-frame', out.name, '--expect', 's5=0')
            self.check(r)
            self.assertGreater(Path(out.name).stat().st_size, 1_000_000)
            self.assertRegex(r.stderr, rb'GPU [1-9]')

    def test_default_smoke(self):
        candidates = [Path(os.environ.get('CS150_SW', '/workspace/software')), ROOT / '../../software']
        sw = next((p for p in candidates if all((p / rel).is_file() for rel in
                  ['gios/gios.inst.bin', 'bench/bench.inst.bin', 'proj_app7/isr7.bin', 'proj_app7/proj7.inst.bin'])), None)
        if sw is None:
            self.skipTest('optional GIOS/proj7 binary images missing (set CS150_SW)')
        r = run('--sw-dir', sw, '--max-insns', '10000000', '--dump-sm')
        self.check(r)
        self.assertIn(b'PROJ-7', r.stdout)
        self.assertIn(b'fedbeef4', r.stdout.lower())
        self.assertRegex(r.stderr, rb'GPU [1-9]')
        self.assertIn(b'player x 400 y 300', r.stderr)
        r = run('--sw-dir', sw, '--max-insns', '10000000', '--exit-on-halt',
                '--uart-in', 'jal 60000000\\rq', '--dump-sm')
        self.check(r)
        self.assertIn(b'HaltVal = 0x00000001', r.stderr)
        self.assertIn(b'quit 1', r.stderr)

    def test_mhz_rejected_on_non_turbo(self):
        for cpu in ('fast', 'unicorn'):
            with self.subTest(cpu=cpu):
                r = subprocess.run([VV, f'--cpu={cpu}', '--mhz=5', '--no-window', '--bare',
                                    '--max-insns', '1'], capture_output=True, env=ENV)
                self.assertEqual(r.returncode, 2, r.stderr)
                self.assertIn(b'--mhz requires --cpu=turbo', r.stderr)
        # Default backend is fast when --cpu is omitted.
        r = subprocess.run([VV, '--mhz=5', '--no-window', '--bare', '--max-insns', '1'],
                           capture_output=True, env=ENV)
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertIn(b'--mhz requires --cpu=turbo', r.stderr)

    def test_mhz_invalid(self):
        for bad in ('0', '-1', '-0.5', 'abc', ''):
            with self.subTest(bad=bad):
                args = [VV, '--cpu=turbo', '--no-window', f'--mhz={bad}'] if bad else \
                       [VV, '--cpu=turbo', '--no-window', '--mhz=']
                r = subprocess.run(args, capture_output=True, env=ENV)
                self.assertEqual(r.returncode, 2, r.stderr)
                self.assertTrue(b'--mhz' in r.stderr)

    def test_mhz_throttle_wall_time(self):
        # Spin forever (beq $0,$0,-1; nop); --max-insns cuts the run.
        # 1e6 insns at 5 MHz => ~0.2 s. Also confirm unconstrained stays fast.
        import time
        with tempfile.NamedTemporaryFile(suffix='.bin') as f:
            f.write(struct.pack('>2I', 0x1000ffff, 0x00000000)); f.flush()
            base = ['--no-bios', '--load', f'{f.name}@0x60000000', '--entry', '0x60000000',
                    '--max-insns', '1000000']
            t0 = time.monotonic()
            r = run(*base)
            unc = time.monotonic() - t0
            self.check(r)
            self.assertRegex(r.stderr, rb'\[turbo\] insns 1000000')
            self.assertLess(unc, 0.15, f'unconstrained 1e6 insns took {unc:.3f}s')

            t0 = time.monotonic()
            r = run(*base, '--mhz=5')
            wall = time.monotonic() - t0
            self.check(r)
            self.assertIn(b'mhz=5', r.stderr)
            self.assertRegex(r.stderr, rb'\[turbo\] insns 1000000')
            # Expect ~0.2s; allow scheduler jitter but reject unconstrained speeds and multi-second stalls.
            self.assertGreaterEqual(wall, 0.15, f'throttled wall {wall:.3f}s too fast')
            self.assertLess(wall, 1.0, f'throttled wall {wall:.3f}s too slow')
            m = re.search(rb'in ([0-9.]+) s: ([0-9.]+) Mcyc/s', r.stderr)
            self.assertIsNotNone(m, r.stderr)
            observed = float(m.group(2))
            self.assertGreater(observed, 3.5)
            self.assertLess(observed, 7.0)

    @unittest.skipUnless(shutil.which('xvfb-run'), 'optional xvfb-run missing')
    def test_window(self):
        p, addr = prog('loop')
        r = subprocess.run(['xvfb-run', '-a', VV, '--cpu=turbo', '--window', '--no-bios',
                            '--load', f'{p}@{addr:#x}', '--entry', hex(addr), '--expect', 't0=30'],
                           capture_output=True, stdin=subprocess.DEVNULL, env=ENV, timeout=30)
        self.check(r)
        self.assertIn(b'+ display', r.stderr + r.stdout)


if __name__ == '__main__':
    warnings.filterwarnings('ignore', category=ResourceWarning)
    unittest.main(verbosity=2)
