#!/usr/bin/env python3
"""Isolated real server + fake model, and PTY tests for the session UI.

All session/log files live in a temporary directory. The fake model implements
only ai_client_stream so no running model or fixed HTTP port is needed.
"""
import fcntl
import importlib.util
import os
from pathlib import Path
import pty
import queue
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
CLIENT = Path(sys.argv.pop(1) if len(sys.argv) > 1 else ROOT / 'bin/client').resolve()

# Reuse the VT display oracle, extending it for full-screen operations below.
sys.dont_write_bytecode = True
saved_argv = sys.argv[:]
sys.argv = [sys.argv[0]]
spec = importlib.util.spec_from_file_location('input_tests', ROOT / 'tests/test_client_input.py')
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)
sys.argv = saved_argv


class Screen(base.Screen):
    def __init__(self, columns=100, rows=20):
        super().__init__(columns, rows)
        self.cells[0][:14] = list('ORIGINAL SHELL') + [' ']
        self.row = 1
        self.original = None
        self.alternate = False
        self.entered = self.exited = False

    def feed(self, data):
        # Decode once, then pass complete non-control characters to base oracle.
        for ch in self.decoder.decode(data):
            if self.escape is not None:
                self.escape += ch
                if self.escape == '\x1b[':
                    continue
                if len(self.escape) >= 3 and '@' <= ch <= '~':
                    sequence = self.escape
                    self.escape = None
                    argument = sequence[2:-1]
                    if sequence == '\x1b[?1049h':
                        self.original = ([r[:] for r in self.cells], self.row, self.column)
                        self.cells = [[' '] * self.columns for _ in range(self.rows)]
                        self.row = self.column = 0
                        self.alternate = self.entered = True
                    elif sequence == '\x1b[?1049l':
                        if self.original:
                            self.cells, self.row, self.column = self.original
                        self.alternate = False
                        self.exited = True
                    elif ch == 'H':
                        values = [int(x or '1') for x in argument.split(';')]
                        self.row = max(0, min(self.rows - 1, values[0] - 1))
                        self.column = max(0, min(self.columns - 1, (values[1] if len(values) > 1 else 1) - 1))
                    elif sequence == '\x1b[2J':
                        self.cells = [[' '] * self.columns for _ in range(self.rows)]
                    else:
                        # The base oracle maintains its own incremental decoder;
                        # only ASCII complete sequences go through it here.
                        super().feed(sequence.encode())
                continue
            if ch == '\x1b':
                self.escape = ch
            else:
                # Feed a whole Unicode character through a fresh decoder, avoiding
                # recursive use of the already-active incremental decoder.
                import codecs
                decoder = self.decoder
                self.decoder = codecs.getincrementaldecoder('utf-8')('replace')
                super().feed(ch.encode())
                self.decoder = decoder


def escape(text):
    return text.replace('\\', '\\\\').replace('\n', '\\n').replace('\r', '\\r').encode()


def unescape(text):
    result = bytearray()
    i = 0
    while i < len(text):
        if text[i] == 92:
            i += 1
            result.extend({92: b'\\', 110: b'\n', 114: b'\r'}[text[i]])
        else:
            result.append(text[i])
        i += 1
    return result.decode('utf-8', errors='replace')


class Wire:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=5)
        self.file = self.sock.makefile('rb')

    def send(self, line):
        self.sock.sendall(line + b'\n')

    def line(self):
        line = self.file.readline()
        if not line:
            raise AssertionError('unexpected EOF')
        return line.rstrip(b'\n')

    def listing(self):
        self.send(b'SESSIONS')
        assert self.line() == b'SESSIONS_BEGIN'
        result = []
        while True:
            line = self.line()
            if line == b'SESSIONS_END':
                return result
            assert line.startswith(b'SESSION '), line
            fields = line.split(b' ', 4)
            result.append((int(fields[1]), int(fields[3]), unescape(fields[4])))

    def open(self, ident=None):
        self.send(b'NEW' if ident is None else f'OPEN {ident}'.encode())
        line = self.line()
        if line.startswith(b'ERROR '):
            return None, unescape(line[6:])
        assert line.startswith(b'OPENED '), line
        ident = int(line.split(b' ', 2)[1])
        history = []
        while True:
            line = self.line()
            if line == b'HISTORY_END':
                return ident, history
            assert line.startswith(b'MESSAGE '), line
            role, context = line.split(b' ')[1:]
            chunks = []
            while True:
                line = self.line()
                if line == b'MESSAGE_END':
                    break
                assert line.startswith(b'TEXT '), line
                chunks.append(unescape(line[5:]))
            history.append((role.decode(), int(context), ''.join(chunks)))

    def leave(self):
        self.send(b'LEAVE')
        assert self.line() == b'LEFT'

    def query(self, text):
        self.send(b'Q ' + escape(text))
        result = ''
        while True:
            line = self.line()
            if line == b'BEGIN':
                continue
            if line == b'END':
                return True, result
            if line.startswith(b'ERROR '):
                return False, result
            assert line.startswith(b'DATA '), line
            result += unescape(line[5:])

    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.file.close()
        self.sock.close()


MODEL_STUB = r'''
#include "ai_client.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
int ai_client_stream(const ai_client_config_t *config, const session_t *s,
                     ai_chunk_callback callback, void *data, char *error, size_t capacity) {
    (void)config;
    const char *q = session_message_at(s, session_count(s)-1)->content;
    if (!strcmp(q, "FAIL")) {
        callback("partial", 7, data);
        snprintf(error, capacity, "fake model failure"); return -1;
    }
    if (!strcmp(q, "LONG")) {
        const char *chunk = "history line: abcdefghijklmnopqrstuvwxyz\n";
        for (int i=0; i<600; ++i) if (callback(chunk, strlen(chunk), data)) return -1;
        return 0;
    }
    if (!strcmp(q, "SLOW")) {
        struct timespec delay = {0, 50000000};
        for (int i=0; i<40; ++i) {
            if (callback("streaming line\n", 15, data)) return -1;
            nanosleep(&delay, NULL);
        }
        return 0;
    }
    char count[40];
    snprintf(count, sizeof(count), "context=%zu|", session_count(s));
    if (callback(count, strlen(count), data)) return -1;
    for (size_t i=0; i<session_count(s); ++i) {
        const session_message_t *m = session_message_at(s, i);
        if (!strcmp(m->role, "user")) {
            if (callback(m->content, strlen(m->content), data) || callback("|",1,data)) return -1;
        }
    }
    return 0;
}
'''


class UITerminal:
    def __init__(self, port):
        self.master, self.slave = pty.openpty()
        self.original = termios.tcgetattr(self.slave)
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack('HHHH', 20, 100, 0, 0))
        self.screen = Screen()
        self.output = bytearray()
        self.process = subprocess.Popen([str(CLIENT), '127.0.0.1', str(port)],
                                        stdin=self.slave, stdout=self.slave, stderr=self.slave,
                                        env={**os.environ, 'TERM': 'xterm-256color', 'LC_ALL': 'C.UTF-8'})
        self.until(lambda: 'Conversations' in self.screen.text() and 'Loading...' not in self.screen.text())

    def pump(self):
        if select.select([self.master], [], [], .02)[0]:
            data = os.read(self.master, 65536)
            self.output.extend(data)
            self.screen.feed(data)

    def until(self, predicate, timeout=8):
        deadline = time.monotonic() + timeout
        while not predicate():
            if time.monotonic() > deadline:
                raise AssertionError(f'UI timeout\n{self.screen.text()}\n{bytes(self.output[-100:])!r}')
            self.pump()
        self.pump()

    def send(self, text):
        os.write(self.master, text.encode() if isinstance(text, str) else text)

    def settle(self):
        deadline = time.monotonic() + .15
        while time.monotonic() < deadline:
            self.pump()

    def close(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            self.until(lambda: self.process.poll() is not None)
        self.pump()
        assert self.screen.exited and not self.screen.alternate
        assert 'ORIGINAL SHELL' in self.screen.text()
        assert termios.tcgetattr(self.slave) == self.original
        os.close(self.master)
        os.close(self.slave)


class SessionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='session-tests-build-')
        directory = Path(cls.build.name)
        stub = directory / 'fake_model.c'
        stub.write_text(MODEL_STUB)
        cls.server_binary = directory / 'server'
        sources = [ROOT/'src'/name for name in ['server.c', 'protocol.c', 'session.c', 'session_store.c', 'logger.c', 'server_log.c']]
        subprocess.run(['gcc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-D_XOPEN_SOURCE=700',
                        '-Wall', '-Wextra', '-Wpedantic', '-I'+str(ROOT/'include'),
                        *map(str, sources), str(stub), '-o', str(cls.server_binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='session-tests-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root/'bin').mkdir()
        (self.root/'logs').mkdir()
        shutil.copy2(self.server_binary, self.root/'bin/server')
        legacy = ('[2026-10-07 10:00:00]\n\nUSER:\norphan\n\n'
                  '[2026-10-07 10:00:01]\n\nUSER:\nold question\nsecond line\n\n'
                  '[2026-10-07 10:00:02]\n\nASSISTANT:\nold answer\n\n')
        (self.root/'logs/session_1.log').write_text(legacy)
        self.legacy_bytes = (self.root/'logs/session_1.log').read_bytes()
        self.start_server()

    def start_server(self):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            self.port = sock.getsockname()[1]
        self.server = subprocess.Popen([str(self.root/'bin/server'), str(self.port)],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        self.addCleanup(self.stop_server)
        assert select.select([self.server.stdout], [], [], 5)[0], 'server did not start'
        line = self.server.stdout.readline()
        assert b'listening' in line, (line, self.server.stderr.read())

    def stop_server(self):
        if self.server.poll() is None:
            os.killpg(self.server.pid, signal.SIGTERM)
            self.server.wait(timeout=5)
        self.server.stdout.close()
        self.server.stderr.close()

    def wire(self):
        wire = Wire(self.port)
        self.addCleanup(wire.close)
        return wire

    def ui(self):
        ui = UITerminal(self.port)
        self.addCleanup(ui.close)
        return ui

    def test_import_once_context_and_original_log_untouched(self):
        wire = self.wire()
        listing = wire.listing()
        self.assertEqual(len(listing), 1)
        ident, history = wire.open(listing[0][0])
        self.assertEqual(history, [('U', 0, 'orphan'), ('U', 1, 'old question\nsecond line'), ('A', 1, 'old answer')])
        ok, reply = wire.query('continue')
        self.assertTrue(ok)
        self.assertNotIn('orphan', reply)
        self.assertIn('old question\nsecond line', reply)
        wire.leave()
        self.stop_server()
        self.start_server()
        wire = self.wire()
        self.assertEqual(len(wire.listing()), 1)
        self.assertEqual(len(wire.open(ident)[1]), 5)
        self.assertEqual((self.root/'logs/session_1.log').read_bytes(), self.legacy_bytes)

    def test_exclusive_open_release_and_disconnect(self):
        a, b = self.wire(), self.wire()
        ident, _ = a.open()
        self.assertEqual(dict((i, busy) for i, busy, _ in b.listing())[ident], 1)
        self.assertEqual(b.open(ident), (None, 'session is in use'))
        a.leave()
        self.assertEqual(b.open(ident)[0], ident)
        b.close()
        # Cleanup remains idempotent and lock release follows child teardown.
        for _ in range(100):
            opened, history = a.open(ident)
            if opened is not None:
                break
            time.sleep(.01)
        self.assertEqual(opened, ident)

    def test_creation_race_unique_ids(self):
        barrier = threading.Barrier(6)
        output = queue.Queue()
        def create():
            wire = Wire(self.port)
            try:
                barrier.wait()
                output.put(wire.open()[0])
            finally:
                wire.close()
        threads = [threading.Thread(target=create) for _ in range(6)]
        for thread in threads: thread.start()
        for thread in threads: thread.join(timeout=5)
        ids = [output.get(timeout=2) for _ in threads]
        self.assertEqual(len(set(ids)), 6)

    def test_context_limit_failure_rollback_and_full_history(self):
        wire = self.wire()
        ident, _ = wire.open()
        for i in range(20):
            self.assertTrue(wire.query(f'question{i}')[0])
        self.assertFalse(wire.query('FAIL')[0])
        wire.leave()
        _, history = wire.open(ident)
        self.assertEqual(len(history), 40)
        self.assertFalse(any(text == 'FAIL' for _, _, text in history))
        ok, reply = wire.query('next')
        self.assertTrue(ok)
        self.assertIn('context=31|', reply)
        self.assertNotIn('question0|', reply)
        self.assertIn('question5|', reply)
        self.assertNotIn('FAIL', reply)

    def test_long_history_transfer_and_session_isolation(self):
        wire = self.wire()
        ident, _ = wire.open()
        ok, reply = wire.query('LONG')
        self.assertTrue(ok)
        self.assertGreater(len(reply), 8192)
        wire.leave()
        self.assertEqual(wire.open(ident)[1][-1][2], reply)
        wire.leave()
        wire.open()
        self.assertEqual(wire.query('fresh'), (True, 'context=1|fresh|'))

    def test_save_failure_does_not_commit(self):
        wire = self.wire()
        ident, _ = wire.open()
        wire.query('before')
        path = self.root/f'sessions/session_{ident}.dat'
        original = path.read_bytes()
        path.unlink()
        path.mkdir()  # force atomic rename to fail even when tests run as root
        self.assertFalse(wire.query('must not persist')[0])
        path.rmdir()
        path.write_bytes(original)
        wire.leave()
        history = wire.open(ident)[1]
        self.assertEqual(len(history), 2)
        self.assertNotIn('must not persist', wire.query('after')[1])

    def test_corruption_and_invalid_id_keep_connection_usable(self):
        wire = self.wire()
        ident, _ = wire.open()
        wire.leave()
        (self.root/f'sessions/session_{ident}.dat').write_bytes(b'broken')
        self.assertIsNone(wire.open(ident)[0])
        self.assertIsNone(wire.open('../logs')[0])
        self.assertIsNone(wire.open(99999)[0])
        self.assertIsNotNone(wire.open()[0])

    def test_legacy_client_lazy_session(self):
        wire = self.wire()
        self.assertEqual(wire.query('legacy'), (True, 'context=1|legacy|'))
        listing = wire.listing()
        self.assertEqual(len(listing), 2)
        self.assertTrue(any(title == 'legacy' for _, _, title in listing))

    def test_ui_new_chat_back_and_history_restore(self):
        ui = self.ui()
        self.assertTrue(ui.screen.entered)
        self.assertNotIn('ORIGINAL SHELL', ui.screen.text())
        ui.send('n')
        ui.until(lambda: 'Conversation 2' in ui.screen.text() and 'Loading...' not in ui.screen.text())
        self.assertNotIn('old question', ui.screen.text())
        ui.send('甲乙\x1b[D\x7f中\x1b[C\nsecond\r')
        ui.until(lambda: 'context=1|' in ui.screen.text() and 'Receiving reply' not in ui.screen.text())
        self.assertIn('中乙', ui.screen.text())
        ui.send('/back\r')
        ui.until(lambda: 'Conversations' in ui.screen.text() and 'Loading...' not in ui.screen.text())
        self.assertNotIn('AI:', ui.screen.text())
        ui.send('\r')
        ui.until(lambda: 'Conversation 2' in ui.screen.text() and 'context=1|' in ui.screen.text())
        self.assertNotIn('old answer', ui.screen.text())
        ui.send('/quit\r')
        ui.until(lambda: ui.process.poll() is not None)

    def test_ui_import_scroll_resize_and_streaming(self):
        wire = self.wire()
        ident, _ = wire.open()
        wire.query('LONG')
        wire.leave()
        ui = self.ui()
        ui.send('\r')
        ui.until(lambda: f'Conversation {ident}' in ui.screen.text() and 'Loading...' not in ui.screen.text())
        self.assertIn('history line:', ui.screen.text())
        ui.send('\x1b[5~')
        ui.settle()
        self.assertIn('history line:', ui.screen.text())
        ui.send('SLOW\r')
        ui.until(lambda: 'Receiving reply' in ui.screen.text())
        ui.send('\x1b[5~')
        ui.settle()
        fcntl.ioctl(ui.slave, termios.TIOCSWINSZ, struct.pack('HHHH', 10, 50, 0, 0))
        ui.screen = Screen(50, 10)
        # Keep alternate-buffer bookkeeping when changing the oracle geometry.
        ui.screen.alternate = ui.screen.entered = True
        ui.screen.original = ([list('ORIGINAL SHELL'.ljust(50))] + [[' ']*50 for _ in range(9)], 0, 0)
        ui.process.send_signal(signal.SIGWINCH)
        ui.until(lambda: f'Conversation {ident}' in ui.screen.text())
        ui.until(lambda: 'Receiving reply' not in ui.screen.text(), timeout=8)
        ui.send('\x1b[6~' * 100)
        ui.settle()
        self.assertIn('streaming line', ui.screen.text())

    def test_ui_failed_turn_and_busy_error(self):
        wire = self.wire()
        ident = wire.listing()[0][0]
        wire.open(ident)
        ui = self.ui()
        self.assertIn('[in use]', ui.screen.text())
        ui.send('\r')
        ui.until(lambda: 'session is in use' in ui.screen.text())
        ui.send('n')
        ui.until(lambda: 'Conversation 2' in ui.screen.text() and 'Loading...' not in ui.screen.text())
        ui.send('FAIL\r')
        ui.until(lambda: '[not saved]' in ui.screen.text())
        self.assertIn('partial', ui.screen.text())
        ui.send('/back\r')
        ui.until(lambda: 'Conversations' in ui.screen.text() and 'Loading...' not in ui.screen.text())
        ui.send('\r')
        ui.until(lambda: 'Conversation 2' in ui.screen.text() and 'Loading...' not in ui.screen.text())
        self.assertNotIn('partial', ui.screen.text())

    def test_ui_control_sequences_and_signals_restore_screen(self):
        wire = self.wire()
        ident, _ = wire.open()
        wire.query('safe\x1b[2Jtext\x1b]0;bad\x07')
        wire.leave()
        ui = self.ui()
        ui.send('\r')
        ui.until(lambda: f'Conversation {ident}' in ui.screen.text() and 'Loading...' not in ui.screen.text())
        self.assertIn('safe?[2Jtext?]0;bad?', ui.screen.text())
        ui.process.send_signal(signal.SIGINT)
        ui.until(lambda: ui.process.poll() is not None)

    def test_ui_pasted_menu_text_does_not_create_sessions(self):
        ui = self.ui()
        ui.send(b"\x1b[200~nnnq\r\x1b[201~")
        ui.settle()
        self.assertIn('Conversations', ui.screen.text())
        self.assertIsNone(ui.process.poll())
        self.assertEqual(len(self.wire().listing()), 1)

    def test_ui_tiny_window_restores_editor_after_resize(self):
        ui = self.ui()
        ui.send('n')
        ui.until(lambda: 'Conversation 2' in ui.screen.text() and 'Loading...' not in ui.screen.text())
        ui.send('draft')
        ui.settle()
        fcntl.ioctl(ui.slave, termios.TIOCSWINSZ, struct.pack('HHHH', 3, 15, 0, 0))
        ui.process.send_signal(signal.SIGWINCH)
        ui.until(lambda: 'Window too' in ui.screen.text())
        fcntl.ioctl(ui.slave, termios.TIOCSWINSZ, struct.pack('HHHH', 20, 100, 0, 0))
        ui.process.send_signal(signal.SIGWINCH)
        ui.until(lambda: 'you> draft' in ui.screen.text())

    def test_fragmented_frames_keep_keyboard_responsive(self):
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        self.addCleanup(listener.close)
        half_frame = threading.Event()
        errors = queue.Queue()
        def peer():
            try:
                connection, _ = listener.accept()
                with connection, connection.makefile('rb') as stream:
                    connection.settimeout(5)
                    assert stream.readline() == b'SESSIONS\n'
                    connection.sendall(b'SESSIONS_BEGIN\nSESSIONS_END\n')
                    assert stream.readline() == b'NEW\n'
                    for part in [b'OPEN', b'ED 9 Fr', b'agmented\nHISTORY_', b'END\n']:
                        connection.sendall(part)
                        time.sleep(.02)
                    assert stream.readline().startswith(b'Q ')
                    connection.sendall(b'BEGIN\nDATA incomplete')
                    half_frame.set()
                    assert stream.readline() == b'QUIT\n'
            except Exception as error:
                errors.put(error)
        thread = threading.Thread(target=peer, daemon=True)
        thread.start()
        ui = UITerminal(listener.getsockname()[1])
        self.addCleanup(ui.close)
        ui.send('n')
        ui.until(lambda: 'Fragmented' in ui.screen.text() and 'Loading...' not in ui.screen.text())
        ui.send('hello\r')
        ui.until(half_frame.is_set)
        ui.send(b'\x03')
        ui.until(lambda: ui.process.poll() is not None)
        thread.join(timeout=5)
        self.assertFalse(thread.is_alive())
        self.assertTrue(errors.empty(), list(errors.queue))


    def test_ui_connection_loss_restores_screen(self):
        ui = self.ui()
        self.stop_server()
        ui.until(lambda: ui.process.poll() is not None)
        self.assertNotEqual(ui.process.returncode, 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
