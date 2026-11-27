"""The bridge's output pacing at the serial speeds, including 115,200.

--baud paces raw output to about 90% of the line rate (10 bits per byte), so the
WiFi232's buffer doesn't fill. 115,200 needs no bridge change; this holds that
down. Run with:  python3 -m unittest discover -s tests
"""
import asyncio
import os
import sys
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import bridge  # noqa: E402


class PacingTests(unittest.TestCase):
    def test_rate_is_ninety_percent_of_the_line(self):
        self.assertAlmostEqual(bridge.Link(115200).rate, 10368.0)
        self.assertAlmostEqual(bridge.Link(57600).rate, 5184.0)
        self.assertAlmostEqual(bridge.Link(19200).rate, 1728.0)

    def test_zero_baud_does_not_pace(self):
        self.assertEqual(bridge.Link(0).rate, 0)

    def test_the_default_is_still_57600(self):
        # run.sh passes options straight through, so --baud 115200 is enough.
        src = open(bridge.__file__).read()
        self.assertRegex(src, r'--baud"?,\s*type=int,\s*default=57600')

    def test_a_burst_takes_about_as_long_as_the_line_needs(self):
        sent = []
        link = bridge.Link(115200)
        data = b"x" * 3200          # 50 chunks of 64; the first goes out at once

        async def run():
            with mock.patch.object(bridge.os, "write", lambda fd, chunk: sent.append(chunk)):
                t0 = time.monotonic()
                await link.send(data)
                return time.monotonic() - t0

        elapsed = asyncio.run(run())
        self.assertEqual(b"".join(sent), data)
        want = (len(data) - 64) / 10368.0       # about 0.30 s
        self.assertGreater(elapsed, want * 0.8)
        self.assertLess(elapsed, want * 2.0)


if __name__ == "__main__":
    unittest.main()
