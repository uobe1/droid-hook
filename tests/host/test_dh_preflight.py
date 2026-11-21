#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host tests for dh_preflight() and dh_probe_protection()."""

import subprocess
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HARNESS = pathlib.Path(__file__).resolve().parent / 'dh_preflight_harness.c'
BIN = pathlib.Path('/tmp/dh_preflight_harness')

DH_OK, DH_E_INVAL, DH_E_BUSY, DH_E_NOENT, DH_E_OPNOTSUPP, DH_E_VERIFY = 0, -22, -16, -2, -95, -1000


def build():
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'include'),
                    str(HARNESS), str(ROOT / 'src' / 'dh_rw.c'),
                    str(ROOT / 'src' / 'dh_txn.c'),
                    str(ROOT / 'src' / 'dh_preflight.c'),
                    '-o', str(BIN)], check=True, capture_output=True, text=True)


def run(cmd):
    out = subprocess.run([str(BIN), cmd], check=True,
                         capture_output=True, text=True).stdout
    d = {}
    for line in out.splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            d[k] = int(v)
    return d


class PreflightTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build()

    def test_preflight_ok(self):
        self.assertEqual(run('preflight_ok')['preflight'], DH_OK)

    def test_preflight_rejects_open_transaction(self):
        self.assertEqual(run('preflight_busy')['preflight'], DH_E_BUSY)

    def test_preflight_rejects_abi_mismatch(self):
        self.assertEqual(run('preflight_bad_abi')['preflight'], DH_E_INVAL)

    def test_preflight_rejects_null_ctx(self):
        self.assertEqual(run('preflight_null')['preflight_null_ctx'], DH_E_INVAL)

    def test_preflight_rejects_uninitialized_ctx(self):
        self.assertEqual(run('preflight_uninit')['preflight_uninit'], DH_E_INVAL)

    def test_probe_reports_fixmap_route_and_keeps_bytes(self):
        d = run('probe_ok')
        self.assertEqual(d['probe'], DH_OK)
        self.assertEqual(d['route'], 3)          # DH_ROUTE_FIXMAP
        self.assertEqual(d['bytes_kept'], 1)     # same-byte write changed nothing

    def test_probe_reports_opnotsupp_without_ro_route(self):
        self.assertEqual(run('probe_no_ro')['probe'], DH_E_OPNOTSUPP)

    def test_probe_detects_a_corrupting_write(self):
        self.assertEqual(run('probe_corrupt')['probe'], DH_E_VERIFY)

    def test_probe_rejects_null_addr(self):
        self.assertEqual(run('probe_null_addr')['probe'], DH_E_INVAL)

    def test_probe_propagates_a_real_read_error(self):
        # A failing read must surface the backend's errno, never DH_OK.
        self.assertEqual(run('probe_read_fail')['probe'], DH_E_NOENT)


if __name__ == '__main__':
    unittest.main()
