"""bridge.py --demo with a demo session that fails on purpose, for the
tests of what happens when a model or effort change cannot be applied.
SE30_FAIL picks the failure:
    set_model   set_model raises (the bridge should restart instead)
    restart     set_model raises, and so does a start with an effort set
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import bridge  # noqa: E402

FAIL = os.environ.get("SE30_FAIL", "")
_start = bridge.DemoSession.start


async def set_model(self, model):
    raise RuntimeError("set_model refused")


async def start(self):
    if FAIL == "restart" and self.cfg.effort:
        raise RuntimeError("start refused")
    await _start(self)


bridge.DemoSession.set_model = set_model
bridge.DemoSession.start = start

if __name__ == "__main__":
    bridge.main()
