"""Context and cost per conversation: the usage meter, how the session feeds
it from the Claude CLI's messages, and the wire behavior against
bridge.py --demo. Run with:
    python3 -m unittest discover -s tests
"""
import asyncio
import os
import shutil
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge  # noqa: E402
from claude_agent_sdk import AssistantMessage, ResultMessage, TextBlock  # noqa: E402
from test_bridge_agents import FakeTerm, StubClient, attach, sub_use  # noqa: E402
from test_bridge_resume import Base  # noqa: E402
from v2client import Client  # noqa: E402

WINDOW = {"m": {"contextWindow": 200000}}


def answer(used_parts=(1000, 2000, 80000, 1112), model="m", parent=None):
    keys = ("input_tokens", "cache_creation_input_tokens", "cache_read_input_tokens",
            "output_tokens")
    return AssistantMessage(content=[TextBlock(text="ok")], model=model,
                            parent_tool_use_id=parent, usage=dict(zip(keys, used_parts)))


def finish(cost, model_usage=WINDOW, subtype="success"):
    return ResultMessage(subtype=subtype, duration_ms=1, duration_api_ms=1, is_error=False,
                         num_turns=1, session_id="s", total_cost_usd=cost,
                         model_usage=model_usage)


class MeterTests(unittest.TestCase):
    def feed(self, m, cost, used=(0, 0, 0, 1000)):
        m.answer(dict(zip(("input_tokens", "cache_creation_input_tokens",
                           "cache_read_input_tokens", "output_tokens"), used)), "m")
        m.result(cost, WINDOW)

    def test_process_totals_become_turn_costs_and_a_running_total(self):
        m = bridge.UsageMeter()
        costs = []
        for total in (0.01, 0.03, 0.06):
            self.feed(m, total)
            costs.append(round(m.turn_cost, 4))
        self.assertEqual(costs, [0.01, 0.02, 0.03])
        self.assertAlmostEqual(m.total, 0.06)
        self.assertEqual(m.turns, 3)

    def test_a_new_process_counts_from_zero_and_keeps_the_total(self):
        m = bridge.UsageMeter()
        self.feed(m, 0.50)
        m.new_process()
        self.feed(m, 0.02)
        self.assertAlmostEqual(m.turn_cost, 0.02)
        self.assertAlmostEqual(m.total, 0.52)

    def test_a_reset_the_bridge_missed_never_subtracts(self):
        m = bridge.UsageMeter()
        self.feed(m, 0.06)
        self.feed(m, 0.02)
        self.assertAlmostEqual(m.turn_cost, 0.02)
        self.assertAlmostEqual(m.total, 0.08)

    def test_context_is_input_cache_and_output_over_the_models_window(self):
        m = bridge.UsageMeter()
        self.feed(m, 0.01, (1000, 2000, 80000, 1112))
        self.assertEqual((m.used, m.window, m.percent()), (84112, 200000, 42))
        self.assertEqual(m.wire(), "84112 200000 0.0100")

    def test_the_window_is_found_by_model_or_canonical_name_and_never_guessed(self):
        m = bridge.UsageMeter()
        m.answer({"input_tokens": 10}, "alias")
        m.result(0.0, {"provider-id": {"canonicalModel": "alias", "contextWindow": 1000000}})
        self.assertEqual(m.window, 1000000)
        m.answer({"input_tokens": 10}, "alias")
        m.result(0.0, {"other": {"contextWindow": 200000}})
        self.assertEqual((m.used, m.window, m.percent()), (10, None, None))
        self.assertEqual(m.wire(), "10 - 0.0000")

    def test_a_turn_without_usage_keeps_the_reading_and_still_counts_cost(self):
        m = bridge.UsageMeter()
        self.feed(m, 0.01)
        before = (m.used, m.window)
        m.result(0.03, None)
        self.assertEqual((m.used, m.window), before)
        self.assertAlmostEqual(m.total, 0.03)

    def test_unknown_cost_is_a_dash_and_nothing_known_says_nothing(self):
        m = bridge.UsageMeter()
        self.assertIsNone(m.wire())
        m.answer({"output_tokens": 5}, "m")
        m.result(None, WINDOW)
        self.assertIsNone(m.turn_cost)
        self.assertEqual(m.wire(), "5 200000 -")

    def test_garbage_usage_is_ignored(self):
        m = bridge.UsageMeter()
        m.answer({"input_tokens": "lots"}, "m")
        m.answer("nope", "m")
        m.answer(None, "m")
        m.result(0.01, WINDOW)
        self.assertIsNone(m.used)

    def test_a_record_round_trips_and_an_old_record_loads_empty(self):
        m = bridge.UsageMeter()
        for total in (0.1, 0.2, 0.3):
            self.feed(m, total)
        again = bridge.UsageMeter.from_record(m.to_record())
        self.assertEqual((again.total, again.turns, again.used, again.window, again.cost_known),
                         (m.total, 3, m.used, m.window, True))
        for rec in (None, {}, {"cost": "x", "turns": -1, "used": 0}, "junk"):
            old = bridge.UsageMeter.from_record(rec)
            self.assertEqual((old.total, old.turns, old.used, old.window, old.wire()),
                             (0.0, 0, None, None, None))


class RecTerm(FakeTerm):
    """Records, in order, what the session says about a result."""

    async def result(self, status, cost, meter=None):
        self.out.append(("result", status, cost))

    async def usage(self, meter):
        self.out.append(("usage", meter.wire()))


class SessionTests(unittest.TestCase):
    def run_session(self, scripts):
        """One session, one turn per script."""
        term = RecTerm()
        cfg = bridge.SessionConfig("/tmp")
        sess = bridge.ClaudeSession(term, cfg)

        async def go():
            attach(sess, StubClient([m for s in scripts[:1] for m in s]))
            await sess.turn("hi")
            for s in scripts[1:]:
                for m in s:
                    sess.client.more.put_nowait(m)
                await sess.turn("again")
            await sess.stop()
        asyncio.run(go())
        return term.out, cfg.meter

    def test_a_turn_sends_its_own_cost_and_then_the_reading(self):
        out, _ = self.run_session([[answer(), finish(0.0123)]])
        self.assertEqual(out[-2:], [("result", "success", 0.0123),
                                    ("usage", "84112 200000 0.0123")])

    def test_a_second_turn_reports_only_its_own_cost(self):
        out, _ = self.run_session([[answer(), finish(0.0123)],
                                   [answer((0, 0, 90000, 10)), finish(0.0400)]])
        results = [o for o in out if o[0] == "result"]
        self.assertAlmostEqual(results[1][2], 0.0277)
        self.assertEqual(out[-1], ("usage", "90010 200000 0.0400"))

    def test_a_subagents_answers_never_change_the_reading(self):
        out, _ = self.run_session([[answer((0, 0, 1000, 0)),
                                    answer((0, 0, 150000, 9), parent="a1"),
                                    sub_use("a1"), finish(0.01)]])
        self.assertEqual(out[-1], ("usage", "1000 200000 0.0100"))

    def test_an_interrupted_turn_keeps_the_reading(self):
        out, meter = self.run_session([[answer(), finish(0.01)],
                                       [finish(0.01, None, "error_during_execution")]])
        self.assertEqual(out[-1], ("usage", "84112 200000 0.0100"))
        self.assertEqual(meter.turns, 2)

    def test_a_restarted_cli_counts_from_zero(self):
        term = RecTerm()
        cfg = bridge.SessionConfig("/tmp")

        async def go():
            first = bridge.ClaudeSession(term, cfg)
            attach(first, StubClient([answer(), finish(0.50)]))
            cfg.meter.new_process()
            await first.turn("one")
            await first.stop()
            second = bridge.ClaudeSession(term, cfg)   # an effort change restarts it
            attach(second, StubClient([answer(), finish(0.02)]))
            cfg.meter.new_process()
            await second.turn("two")
            await second.stop()
        asyncio.run(go())
        self.assertAlmostEqual(cfg.meter.turn_cost, 0.02)
        self.assertAlmostEqual(cfg.meter.total, 0.52)

    def test_a_turn_claude_starts_by_itself_reports_too(self):
        term = RecTerm()
        cfg = bridge.SessionConfig("/tmp")
        sess = bridge.ClaudeSession(term, cfg)

        async def go():
            attach(sess, StubClient([]))
            for m in (answer(), finish(0.01)):
                sess.client.more.put_nowait(m)
            for _ in range(20):
                await asyncio.sleep(0)
            turned = await sess.follow()
            await sess.stop()
            return turned
        self.assertTrue(asyncio.run(go()))
        self.assertEqual(term.out[-1], ("usage", "84112 200000 0.0100"))


class TerminalTests(unittest.TestCase):
    def tty(self, mode):
        t = object.__new__(bridge.Terminal)
        t.mode = mode
        t.skip_output = True
        t.lines_out = []
        sent = []

        async def out_line(s):
            sent.append(s)

        async def tag(k, p):
            sent.append(f"{k} {p}")
        t.out_line, t.tag = out_line, tag
        return t, sent

    def test_tty_result_line_carries_the_context_percent(self):
        t, sent = self.tty("tty")
        m = bridge.UsageMeter()
        m.answer({"input_tokens": 84112}, "m")
        m.result(0.0123, WINDOW)
        asyncio.run(t.result("success", m.turn_cost, m))
        self.assertEqual(sent, ["(success, $0.0123, ctx 42%)"])

    def test_tty_result_line_without_a_reading_is_as_before(self):
        t, sent = self.tty("tty")
        asyncio.run(t.result("success", 0.0123, bridge.UsageMeter()))
        self.assertEqual(sent, ["(success, $0.0123)"])

    def test_v1_r_line_keeps_its_format(self):
        t, sent = self.tty("app")
        m = bridge.UsageMeter()
        m.answer({"input_tokens": 84112}, "m")
        m.result(0.0123, WINDOW)
        asyncio.run(t.result("success", m.turn_cost, m))
        self.assertEqual(sent, ["R success $0.0123"])


def dollars(lines, sid):
    return [l.split()[3] for l in lines if l.startswith(f"{sid}$ ")]


class WireTests(Base):
    def usage_client(self):
        c = self.client()
        c.send("!USAGE")
        return c

    def test_ok_has_the_word_and_nothing_is_sent_without_the_request(self):
        c = self.client()
        self.assertTrue([l for l in c.text() if l.startswith("#OK")][0].endswith(" usage cmds"))
        self.open_new(c, 1, "alpha")
        c.turn(1, "one")
        c.close()
        self.assertEqual(dollars(c.text(), 1), [])
        self.assertTrue(any(l == "1R success $0.0123" for l in c.text()))

    def test_each_turn_adds_a_usage_line_after_r_and_before_ready(self):
        c = self.usage_client()
        self.open_new(c, 1, "alpha")
        start = len(c.lines)
        c.turn(1, "one")
        c.turn(1, "two")
        lines = c.text()[start:]
        self.assertEqual([l for l in lines if l.startswith("1$ ")],
                         ["1$ 12000 200000 0.0123", "1$ 16000 200000 0.0246"])
        i = lines.index("1R success $0.0123")
        self.assertEqual(lines[i + 1], "1$ 12000 200000 0.0123")
        self.assertEqual(lines[i + 2], "1>")
        c.close()

    def test_a_resumed_tab_reports_its_total_before_any_turn(self):
        c = self.usage_client()
        tok = self.open_new(c, 1, "alpha")
        c.turn(1, "one")
        c.turn(1, "two")
        c.close()
        c = self.usage_client()
        start = len(c.lines)
        self.assertEqual(self.resume(c, 1, tok, "alpha"), (tok, "resumed"))
        lines = c.text()[start:]
        self.assertIn("1$ 16000 200000 0.0246", lines)
        self.assertLess(lines.index("1$ 16000 200000 0.0246"), lines.index("1>"))
        start = len(c.lines)
        c.turn(1, "three")
        self.assertIn("1R success $0.0123", c.text()[start:])
        self.assertIn("1$ 20000 200000 0.0369", c.text()[start:])
        c.close()

    def test_a_link_that_drops_right_after_a_result_loses_nothing(self):
        c = self.usage_client()
        tok = self.open_new(c, 1, "alpha")
        c.send("1P one")
        c.wait_for(lambda l: l.startswith("1? "))
        c.send("1A 1 y")
        c.wait_for(lambda l: l.startswith("1$ "))
        c.close()
        c = self.usage_client()
        start = len(c.lines)
        self.resume(c, 1, tok, "alpha")
        self.assertIn("1$ 12000 200000 0.0123", c.text()[start:])
        c.close()

    def test_new_starts_over_and_says_nothing_until_a_turn_runs(self):
        c = self.usage_client()
        self.open_new(c, 1, "alpha")
        c.turn(1, "one")
        c.turn(1, "two")
        start = len(c.lines)
        c.send("1P /new")
        c.wait_line("1I New conversation.", start=start)
        c.wait_line("1>", start=start)
        self.assertEqual(dollars(c.text()[start:], 1), [])
        c.turn(1, "fresh")
        self.assertIn("1$ 12000 200000 0.0123", c.text()[start:])
        c.close()

    def test_full_jumps_the_context_to_eighty_five_percent(self):
        c = self.usage_client()
        self.open_new(c, 1, "alpha")
        start = len(c.lines)
        c.turn(1, "fill it full")
        self.assertIn("1$ 170000 200000 0.0123", c.text()[start:])
        c.close()

    def test_usage_command_prints_context_categories_and_cost(self):
        c = self.usage_client()
        self.open_new(c, 1, "alpha")
        c.turn(1, "one")
        start = len(c.lines)
        c.send("1P /usage")
        c.wait_line("1>", start=start)
        lines = [l[3:] for l in c.text()[start:] if l.startswith("1I ")]
        self.assertEqual(lines[0], "Context: 12,000 of 200,000 tokens (6%).")
        self.assertIn("  System prompt: 8,000", lines)
        self.assertFalse([l for l in lines if "Free" in l])
        self.assertEqual(lines[-1], "Cost: $0.0123 over 1 turn.")
        c.close()

    def test_usage_command_works_without_a_request_and_in_a_chat(self):
        c = self.client()
        start = len(c.lines)
        c.send("1O *")
        c.wait_line("1>", start=start)
        c.send("1P /usage")
        c.wait_for(lambda l: l.startswith("1I Cost:"), start=start)
        lines = [l[3:] for l in c.text()[start:] if l.startswith("1I ")]
        self.assertEqual(lines[0], "Context: no reading yet.")
        self.assertEqual(lines[-1], "Cost: unknown over 0 turns.")
        c.close()


class OtherClientsTests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        os.mkdir(os.path.join(self.root, "alpha"))
        self.addCleanup(shutil.rmtree, self.root, True)

    def client(self, *args):
        c = Client("--demo", "--projects", self.root, "--window", "0", *args)
        self.addCleanup(c.reap)
        return c

    def test_v1_usage_and_new(self):
        c = self.client()
        c.send(b"")
        c.send("#HELLO SE30 1")
        c.wait_line("#OK se30bridge 1")
        i = c.wait_line(">")
        c.send("hi")
        j = c.wait_for(lambda l: l.startswith("? "), 20, i + 1)
        c.send("A 1 y")
        k = c.wait_line("R success $0.0123", 20, j)
        self.assertFalse(any("$" in l[1:2] for l in c.text()))   # v1 has no usage line
        c.send("/usage")
        c.wait_for(lambda l: l.startswith("I Cost:"), 20, k)
        self.assertIn("I Cost: $0.0123 over 1 turn.", c.text())
        c.send("/new")
        c.wait_line("I New conversation.", 20, k)
        n = len(c.lines)
        c.send("/usage")
        c.wait_for(lambda l: l.startswith("I Cost:"), 20, n)
        self.assertIn("I Cost: unknown over 0 turns.", c.text()[n:])
        c.close()

    def test_tty_usage_prints_the_same_lines(self):
        c = self.client("--rows", "100")
        end = time.monotonic() + 20
        while b"Ready." not in bytes(c.raw) and time.monotonic() < end:
            time.sleep(0.05)
        c.send("/usage")
        end = time.monotonic() + 20
        while b"Cost:" not in bytes(c.raw) and time.monotonic() < end:
            time.sleep(0.05)
        raw = bytes(c.raw)
        self.assertIn(b"Context: no reading yet.", raw)
        self.assertIn(b"Cost: unknown over 0 turns.", raw)
        c.close()


if __name__ == "__main__":
    unittest.main()
