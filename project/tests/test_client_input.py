#!/usr/bin/env python3
"""Exercise the real client through a PTY and a local, model-free TCP peer."""
import codecs
import fcntl
import os
import pty
import queue
import select
import signal
import socket
import struct
import subprocess
import sys
import termios
import threading
import time
import unicodedata
import unittest
from pathlib import Path

CLIENT = str(Path(sys.argv.pop(1) if len(sys.argv) > 1 else "bin/client").resolve())


class Screen:
    """Small VT display oracle: cursor motion, erasure, wrapping and scrolling."""
    def __init__(self, columns=80, rows=24):
        self.columns, self.rows = columns, rows
        self.cells = [[" "] * columns for _ in range(rows)]
        self.row = self.column = 0
        self.escape = None
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self.max_up = self.max_down = 0

    def newline(self):
        self.row += 1
        if self.row >= self.rows:
            self.cells.pop(0)
            self.cells.append([" "] * self.columns)
            self.row = self.rows - 1

    def feed(self, data):
        for ch in self.decoder.decode(data):
            if self.escape is not None:
                self.escape += ch
                if self.escape == "\x1b[":
                    continue
                if len(self.escape) >= 3 and "@" <= ch <= "~":
                    code, argument = ch, self.escape[2:-1]
                    number = int(argument or "1") if argument.isdigit() or not argument else 1
                    if code == "A":
                        self.max_up = max(self.max_up, number)
                        self.row = max(0, self.row - number)
                    elif code == "B":
                        self.max_down = max(self.max_down, number)
                        self.row = min(self.rows - 1, self.row + number)
                    elif code == "C":
                        self.column = min(self.columns - 1, self.column + number)
                    elif code == "J":
                        for row in range(self.row, self.rows):
                            start = self.column if row == self.row else 0
                            self.cells[row][start:] = [" "] * (self.columns - start)
                    self.escape = None
                continue
            if ch == "\x1b":
                self.escape = ch
            elif ch == "\r":
                self.column = 0
            elif ch == "\n":
                self.newline()
            elif ord(ch) >= 32:
                width = 0 if unicodedata.combining(ch) else 2 if unicodedata.east_asian_width(ch) in "WF" else 1
                if width == 0:
                    continue
                if self.column + width > self.columns:
                    self.column = 0
                    self.newline()
                self.cells[self.row][self.column] = ch
                if width == 2:
                    self.cells[self.row][self.column + 1] = ""
                self.column += width

    def text(self):
        return "\n".join("".join(row).rstrip() for row in self.cells)


class Peer:
    def __init__(self, reply=True):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.frames = queue.Queue()
        self.connection = None
        self.reply = reply
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        try:
            self.connection, _ = self.listener.accept()
            with self.connection.makefile("rb") as stream:
                for line in stream:
                    self.frames.put(line.rstrip(b"\n"))
                    if line == b"QUIT\n":
                        break
                    if not self.reply:
                        break
                    self.connection.sendall(b"BEGIN\nDATA ok\nEND\n")
        except OSError:
            pass
        finally:
            if self.connection:
                self.connection.close()

    def close(self):
        if self.connection:
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.listener.close()
        self.thread.join(timeout=2)


class Session:
    def __init__(self, columns=80, rows=24, reply=True):
        self.peer = Peer(reply)
        self.master, self.slave = pty.openpty()
        self.original = termios.tcgetattr(self.slave)
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        self.screen = Screen(columns, rows)
        self.output = bytearray()
        self.process = subprocess.Popen(
            [CLIENT, "127.0.0.1", str(self.peer.port)],
            stdin=self.slave, stdout=self.slave, stderr=self.slave,
            env={**os.environ, "TERM": "xterm-256color", "LC_ALL": "C"},
        )
        self.until(lambda: b"\x1b[?2004h" in self.output)

    def pump(self):
        if select.select([self.master], [], [], 0.02)[0]:
            data = os.read(self.master, 65536)
            self.output.extend(data)
            self.screen.feed(data)

    def until(self, condition, timeout=5):
        deadline = time.monotonic() + timeout
        while not condition():
            if time.monotonic() > deadline:
                raise AssertionError(f"client timeout: {bytes(self.output[-500:])!r}")
            self.pump()
        self.pump()

    def send(self, data):
        os.write(self.master, data.encode() if isinstance(data, str) else data)

    def frame(self):
        self.until(lambda: not self.peer.frames.empty())
        return self.peer.frames.get_nowait()

    def next_prompt(self):
        self.until(lambda: b"AI: ok" in self.output and not termios.tcgetattr(self.slave)[3] & termios.ICANON)

    def settle(self):
        deadline = time.monotonic() + 0.15
        while time.monotonic() < deadline:
            self.pump()

    def wait_exit(self):
        self.until(lambda: self.process.poll() is not None)
        if termios.tcgetattr(self.slave) != self.original:
            raise AssertionError("terminal attributes were not restored")
        if b"\x1b[?2004l" not in self.output:
            raise AssertionError("bracketed paste was not disabled")

    def close(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            self.wait_exit()
        self.peer.close()
        os.close(self.master)
        os.close(self.slave)


class ClientInputTests(unittest.TestCase):
    def session(self, **kwargs):
        session = Session(**kwargs)
        self.addCleanup(session.close)
        return session

    def query(self, data, expected, **kwargs):
        session = self.session(**kwargs)
        session.send(data)
        self.assertEqual(session.frame(), expected)
        return session

    def test_middle_insert_arrows_and_ss3(self):
        self.query(b"ac\x1bODb\x1b[C\r", b"Q abc")

    def test_unicode_backspace_delete_and_insert(self):
        self.query("甲乙丙\x1b[D\x7f丁\x1b[3~\r", "Q 甲丁".encode())

    def test_fragmented_unicode_and_escape(self):
        session = self.session()
        for chunk in [b"\xe4", b"\xb8", b"\xad", b"a", b"\x1b[", b"D", b"b", b"\r"]:
            session.send(chunk)
            session.pump()
        self.assertEqual(session.frame(), "Q 中ba".encode())

    def test_shift_enter_variants_and_ctrl_j(self):
        for key in [b"\x1b[13;2u", b"\x1b[27;2;13~", b"\x1b[13;2~", b"\n"]:
            with self.subTest(key=key):
                session = self.session()
                session.send(b"first" + key + b"second")
                session.settle()
                self.assertTrue(session.peer.frames.empty(), "newline sent prematurely")
                session.send(b"\r")
                self.assertEqual(session.frame(), b"Q first\\nsecond")

    def test_vertical_movement_and_goal_column(self):
        # Preserve terminal column 8 across a shorter intermediate line.
        self.query(b"abc\nx\n12345678\x1b[A\x1b[A!\r", b"Q abc!\\nx\\n12345678")
        self.query(b"abc\nx\n12345678\x1b[A\x1b[A\x1b[B\x1b[B!\r", b"Q abc\\nx\\n12345678!")

    def test_home_end_and_newline_deletion(self):
        self.query(b"abc\ndef\x1b[H\x7f\x1b[F!\r", b"Q abcdef!")

    def test_home_end_on_wrapped_display_rows(self):
        self.query(b"abcdef\x1b[H!\x1b[F?\r", b"Q abcd!ef?", columns=10)

    def test_arrows_move_between_wrapped_rows(self):
        self.query(b"abcdef\x1b[A!\r", b"Q !abcdef", columns=10)

    def test_bracketed_paste_normalizes_crlf_and_does_not_send(self):
        session = self.session()
        session.send(b"\x1b[200~one\r\ntwo\rthree\nfour\\end\x1b[201~")
        session.settle()
        self.assertTrue(session.peer.frames.empty())
        session.send(b"\r")
        self.assertEqual(session.frame(), b"Q one\\ntwo\\nthree\\nfour\\\\end")

    def test_unknown_overlong_and_incomplete_sequences(self):
        self.query(b"a\x1b[99;99z\x1b[" + b"1;" * 100 + b"z" + b"b\r", b"Q ab")
        session = self.session()
        session.send(b"a\x1b[")
        session.settle()
        session.send(b"b\r")
        self.assertEqual(session.frame(), b"Q ab")

    def test_escape_followed_by_enter(self):
        self.query(b"a\x1b[\rb\r", b"Q a")

    def test_invalid_unicode_never_enters_message(self):
        self.query(b"a\xc0\xaf\xe0\x80\x80\xf4\x90\x80\x80b\r", b"Q ab")

    def test_exact_input_limit_and_overflow(self):
        session = self.session()
        session.send(b"\x1b[200~" + b"a" * 4095 + b"\x1b[201~\r")
        self.assertEqual(session.frame(), b"Q " + b"a" * 4095)
        session = self.session()
        session.send(b"\x1b[200~" + b"a" * 4094 + "中".encode() + b"\x1b[201~\x7fb\r")
        self.assertEqual(session.frame(), b"Q " + b"a" * 4093 + b"b")

    def test_escaped_frame_limit_keeps_text_editable(self):
        session = self.session()
        session.send(b"\x1b[200~" + b"\\" * 4095 + b"\x1b[201~\r")
        session.settle()
        self.assertTrue(session.peer.frames.empty())
        self.assertIn(b"protocol frame limit", session.output)
        session.send(b"\x7f\r")
        self.assertEqual(session.frame(), b"Q " + b"\\\\" * 4094)

    def test_multiline_redraw_and_unicode_cursor(self):
        session = self.session()
        session.send("甲乙\nxyzabcd\x1b[A")
        session.settle()
        self.assertIn("you> 甲乙\nxyzabcd", session.screen.text())
        self.assertEqual(session.screen.column, 7)  # prompt + one Chinese glyph
        session.send("!\r")
        self.assertEqual(session.frame(), "Q 甲!乙\\nxyzabcd".encode())

    def test_viewport_scroll_and_edit_hidden_lines(self):
        session = self.session(columns=12, rows=4)
        session.send(b"\x1b[200~a\nb\nc\nd\ne\nf\x1b[201~")
        session.settle()
        self.assertIn("d\ne\nf", session.screen.text())
        session.send(b"\x1b[A" * 5 + b"!\r")
        # Column 1 is left of the first row's five-column prompt.
        self.assertEqual(session.frame(), b"Q !a\\nb\\nc\\nd\\ne\\nf")
        self.assertLess(session.screen.max_up, 4)

    def test_resize_without_typing_keeps_text_and_cursor(self):
        session = self.session()
        session.send(b"abcdef\x1b[D")
        session.settle()
        fcntl.ioctl(session.slave, termios.TIOCSWINSZ, struct.pack("HHHH", 4, 10, 0, 0))
        session.screen = Screen(10, 4)
        session.process.send_signal(signal.SIGWINCH)
        session.settle()
        self.assertIn("you> abcd\nef", session.screen.text())
        self.assertEqual(session.screen.column, 1)
        session.send(b"!\r")
        self.assertEqual(session.frame(), b"Q abcde!f")

    def test_ctrl_c_ctrl_d_and_external_signals_restore_terminal(self):
        for key in [b"\x03", b"\x04", b"/quit\r"]:
            with self.subTest(key=key):
                session = self.session()
                session.send(key)
                self.assertEqual(session.frame(), b"QUIT")
                session.wait_exit()
        for sig in [signal.SIGINT, signal.SIGTERM, signal.SIGHUP]:
            with self.subTest(signal=sig):
                session = self.session()
                session.process.send_signal(sig)
                session.wait_exit()

    def test_nonempty_ctrl_d_deletes(self):
        self.query(b"abc\x1b[D\x04\r", b"Q ab")

    def test_connection_error_restores_terminal(self):
        session = self.query(b"hello\r", b"Q hello", reply=False)
        session.wait_exit()
        self.assertIn(b"server closed the connection", session.output)

    def test_multiple_queries_use_restored_modes(self):
        session = self.query(b"first\r", b"Q first")
        session.next_prompt()
        session.send(b"second\nline\r")
        self.assertEqual(session.frame(), b"Q second\\nline")

    def test_pipe_input_and_overlong_line_recovery(self):
        peer = Peer()
        self.addCleanup(peer.close)
        result = subprocess.run([CLIENT, "127.0.0.1", str(peer.port)],
                                input=b"a" * 4096 + b"\nfirst\nlast",
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(peer.frames.get(timeout=2), b"Q first")
        self.assertEqual(peer.frames.get(timeout=2), b"Q last")
        self.assertEqual(peer.frames.get(timeout=2), b"QUIT")
        self.assertIn(b"input exceeds", result.stderr)
        self.assertNotIn(b"\x1b", result.stdout)

    def test_narrow_window_tabs_and_wide_characters(self):
        session = self.query("甲\t乙\n丙\r", "Q 甲\t乙\\n丙".encode(), columns=2, rows=2)
        self.assertLessEqual(session.screen.max_up, 1)
        self.assertLessEqual(session.screen.max_down, 1)

    def test_osc_and_dcs_sequences_are_not_message_text(self):
        self.query(b"a\x1b]0;title\x07\x1bPignored\x1b\\b\r", b"Q ab")

    def test_redirected_output_and_dumb_terminal_use_line_input(self):
        for dumb in [False, True]:
            with self.subTest(dumb=dumb):
                peer = Peer()
                self.addCleanup(peer.close)
                master, slave = pty.openpty()
                self.addCleanup(os.close, master)
                self.addCleanup(os.close, slave)
                original = termios.tcgetattr(slave)
                env = {**os.environ, "TERM": "dumb" if dumb else "xterm-256color"}
                process = subprocess.Popen([CLIENT, "127.0.0.1", str(peer.port)], stdin=slave,
                                           stdout=slave if dumb else subprocess.PIPE,
                                           stderr=subprocess.PIPE, env=env)
                os.write(master, b"hello\n/quit\n")
                _, errors = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 0, errors)
                self.assertEqual(peer.frames.get(timeout=2), b"Q hello")
                self.assertEqual(peer.frames.get(timeout=2), b"QUIT")
                self.assertEqual(termios.tcgetattr(slave), original)


if __name__ == "__main__":
    unittest.main(verbosity=2)
