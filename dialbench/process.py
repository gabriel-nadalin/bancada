"""Process orchestration helpers: spawn two tools and cross-connect their
stdio (a.stdout -> b.stdin, b.stdout -> a.stdin), echoing stderr."""

import subprocess
import sys
import threading
import time


def spawn_pump_pair(a_cmd, b_cmd, a_name, b_name):
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
                dst.write(chunk)
                dst.flush()
        except Exception as e:
            print(f"pump {name}: {e}", file=sys.stderr)

    def echo(src, prefix):
        try:
            for line in src:
                text = line.decode("utf-8", errors="replace").rstrip()
                print(f"[{prefix}] {text}", file=sys.stderr)
        except Exception:
            pass

    threads = [
        threading.Thread(target=pump, args=(a.stdout, b.stdin,
                                            f"{a_name}->{b_name}"),
                         daemon=True),
        threading.Thread(target=pump, args=(b.stdout, a.stdin,
                                            f"{b_name}->{a_name}"),
                         daemon=True),
        threading.Thread(target=echo, args=(a.stderr, a_name), daemon=True),
        threading.Thread(target=echo, args=(b.stderr, b_name), daemon=True),
    ]
    for t in threads:
        t.start()

    return a, b


def wait_cleanup(a, b):
    """Wait until either process exits, then terminate (and kill) both."""
    try:
        while a.poll() is None and b.poll() is None:
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    finally:
        for p in (a, b):
            try:
                p.terminate()
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()
