"""Process orchestration and end-to-end modem data probes."""

import os
import re
import select
import subprocess
import sys
import threading
import time
import tty


class PtyDataProbe:
    """Send commands through the slmodem PTY after the modem connects.

    A probe succeeds only when the far-end command echo, expected response,
    and prompt all return through the modem data path.
    """

    def __init__(self, command="show clock", expected="UTC", prompt="Router>",
                 count=3, max_attempts=5, interval=5.0, settle=3.0,
                 connect_timeout=120.0, response_timeout=15.0,
                 required_rate=None):
        self.command = command
        self.expected = expected
        self.prompt = prompt
        self.count = count
        self.max_attempts = max_attempts
        self.interval = interval
        self.settle = settle
        self.connect_timeout = connect_timeout
        self.response_timeout = response_timeout
        self.required_rate = required_rate
        self.done = threading.Event()
        self.success = False
        self.failure = None
        self.responses = []
        self._pty_ready = threading.Event()
        self._connected = threading.Event()
        self._stop = threading.Event()
        self._pty_path = None
        self._thread = None
        self._rates_seen = []
        self._rate_failure = None

    def observe_modem_stderr(self, line):
        """Observe one decoded stderr line from slmodem_bridge."""
        if line.startswith("PTY: "):
            self._pty_path = line.removeprefix("PTY: ").strip()
            self._pty_ready.set()
        if "modem report result: 1 (CONNECT)" in line:
            self._connected.set()
        match = re.search(
            r"Current data rate: TX=(\d+) RX=(\d+) bit/s", line)
        if match:
            tx_rate, rx_rate = (int(value) for value in match.groups())
            self._rates_seen.append((tx_rate, rx_rate))
            if (self.required_rate is not None and
                    (tx_rate != self.required_rate or
                     rx_rate != self.required_rate)):
                self._rate_failure = (
                    f"data rate changed to TX={tx_rate} RX={rx_rate} bit/s; "
                    f"required {self.required_rate}/{self.required_rate} "
                    "bit/s")

    def start(self):
        if self._thread is not None:
            raise RuntimeError("PTY data probe already started")
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2)

    def raise_for_status(self):
        if self._rate_failure is not None:
            raise RuntimeError(self._rate_failure)
        if not self.success:
            raise RuntimeError(self.failure or "PTY data probe did not finish")

    def _fail(self, message):
        self.failure = message
        print(f"[pty] FAIL: {message}", file=sys.stderr)

    def _wait_event(self, event, deadline):
        while not event.is_set() and not self._stop.is_set():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False
            event.wait(min(0.2, remaining))
        return event.is_set()

    @staticmethod
    def _drain(fd):
        while True:
            readable, _, _ = select.select([fd], [], [], 0)
            if not readable:
                return
            try:
                if not os.read(fd, 4096):
                    return
            except BlockingIOError:
                return

    @staticmethod
    def _write_all(fd, data):
        written = 0
        while written < len(data):
            try:
                count = os.write(fd, data[written:])
                if count == 0:
                    raise BrokenPipeError("PTY write returned zero")
                written += count
            except BlockingIOError:
                select.select([], [fd], [], 0.2)

    def _find_complete_response(self, response):
        command = self.command.encode("utf-8").lower()
        expected = self.expected.encode("utf-8").lower()
        prompt = self.prompt.encode("utf-8").lower()

        folded = bytes(response).lower()
        command_at = folded.find(command)
        if command_at < 0:
            return None
        expected_at = folded.find(expected, command_at + len(command))
        if expected_at < 0:
            return None
        prompt_at = folded.find(prompt, expected_at + len(expected))
        if prompt_at < 0:
            return None
        return prompt_at + len(prompt)

    def _read_response(self, fd, deadline, response):
        complete_at = self._find_complete_response(response)
        if complete_at is not None:
            return complete_at

        while not self._stop.is_set() and time.monotonic() < deadline:
            readable, _, _ = select.select([fd], [], [], 0.2)
            if not readable:
                continue
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                continue
            if not chunk:
                continue
            response.extend(chunk)
            complete_at = self._find_complete_response(response)
            if complete_at is not None:
                return complete_at
        return None

    def _run(self):
        fd = None
        try:
            deadline = time.monotonic() + self.connect_timeout
            if not self._wait_event(self._pty_ready, deadline):
                self._fail("slmodem did not publish its PTY before timeout")
                return

            if not self._wait_event(self._connected, deadline):
                self._fail("modem did not connect before timeout")
                return

            if self._stop.is_set():
                return

            # Open the DTE side only after CONNECT, matching the successful
            # manual control runs and keeping the probe out of call setup and
            # data-pump training.
            fd = os.open(self._pty_path,
                         os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            tty.setraw(fd)

            print(f"[pty] waiting {self.settle:g}s after CONNECT",
                  file=sys.stderr)
            if self._stop.wait(self.settle):
                return

            successes = 0
            pending = bytearray()
            self._drain(fd)
            for attempt in range(1, self.max_attempts + 1):
                payload = f"\r{self.command}\r".encode("utf-8")
                print(f"[pty] TX attempt {attempt}/{self.max_attempts}: "
                      f"{self.command}",
                      file=sys.stderr)
                self._write_all(fd, payload)
                complete_at = self._read_response(
                    fd, time.monotonic() + self.response_timeout, pending)
                if complete_at is None:
                    response = bytes(pending)
                else:
                    response = bytes(pending[:complete_at])
                    del pending[:complete_at]
                decoded = response.decode("utf-8", errors="replace")
                self.responses.append(decoded)
                compact = " | ".join(part for part in decoded.replace(
                    "\r", "").split("\n") if part)
                print(f"[pty] RX attempt {attempt}/{self.max_attempts}: "
                      f"{compact}",
                      file=sys.stderr)

                if complete_at is not None:
                    successes += 1
                    print(f"[pty] response {successes}/{self.count} passed",
                          file=sys.stderr)
                    if successes == self.count:
                        if self.required_rate is not None:
                            if not self._rates_seen:
                                self._fail(
                                    "modem did not report its negotiated "
                                    "data rate")
                                return
                            if self._rate_failure is not None:
                                self._fail(self._rate_failure)
                                return
                        self.success = True
                        print(f"[pty] PASS: {self.count} end-to-end RAS "
                              "responses", file=sys.stderr)
                        return
                else:
                    suffix = "; retrying" if attempt < self.max_attempts else ""
                    print(f"[pty] no complete response on attempt {attempt}"
                          f"{suffix}", file=sys.stderr)

                if attempt < self.max_attempts and self._stop.wait(
                        self.interval):
                    return

            self._fail(f"received {successes} of {self.count} complete "
                       f"responses after {self.max_attempts} attempts")
        except Exception as exc:
            self._fail(str(exc))
        finally:
            if fd is not None:
                os.close(fd)
            self.done.set()


def spawn_pump_pair(a_cmd, b_cmd, a_name, b_name,
                    a_stderr_observer=None, b_stderr_observer=None):
    """Spawn two processes and pump a.stdout->b.stdin, b.stdout->a.stdin.

    Echoes each process's stderr as ``[name] <line>``. Returns (proc_a, proc_b).
    """
    a = subprocess.Popen(
        a_cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, bufsize=0)
    b = subprocess.Popen(
        b_cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, bufsize=0)

    def pump(src, dst, name):
        try:
            while True:
                chunk = src.read(8192)
                if not chunk:
                    break
                pending = memoryview(chunk)
                while pending:
                    requested = len(pending)
                    written = dst.write(pending)
                    if written is None:
                        continue
                    if written == 0:
                        raise BrokenPipeError(
                            f"short write while pumping {name}")
                    if written < requested:
                        print(f"pump {name}: short write {written}/"
                              f"{requested}; retrying remainder",
                              file=sys.stderr)
                    pending = pending[written:]
                dst.flush()
        except Exception as e:
            print(f"pump {name}: {e}", file=sys.stderr)

    def echo(src, prefix, observer):
        try:
            for line in src:
                text = line.decode("utf-8", errors="replace").rstrip()
                print(f"[{prefix}] {text}", file=sys.stderr)
                if observer is not None:
                    observer(text)
        except Exception:
            pass

    threads = [
        threading.Thread(target=pump, args=(a.stdout, b.stdin,
                                            f"{a_name}->{b_name}"),
                         daemon=True),
        threading.Thread(target=pump, args=(b.stdout, a.stdin,
                                            f"{b_name}->{a_name}"),
                         daemon=True),
        threading.Thread(target=echo,
                         args=(a.stderr, a_name, a_stderr_observer),
                         daemon=True),
        threading.Thread(target=echo,
                         args=(b.stderr, b_name, b_stderr_observer),
                         daemon=True),
    ]
    for t in threads:
        t.start()

    return a, b


def wait_cleanup(a, b, completion=None):
    """Wait for a process exit or completion, then terminate both tools."""
    interrupted = False
    try:
        while a.poll() is None and b.poll() is None:
            if completion is not None and completion.done.is_set():
                break
            time.sleep(0.5)
    except KeyboardInterrupt:
        interrupted = True
    finally:
        if completion is not None:
            completion.stop()
        for p in (a, b):
            try:
                p.terminate()
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()
    if completion is not None and not interrupted:
        completion.raise_for_status()
