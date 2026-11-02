# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""TCP access to a simulated board's CLI, for clients without pty support (Java, Windows)."""

from __future__ import annotations

import os
import select
import socket
import threading
import tty


class TcpBridge:
    """Exposes a simulated board's CLI (pty) on a TCP port, for clients without pty support
    (Java, Windows). One client at a time; a new connection replaces the previous one."""

    def __init__(self, pty_path: str, port: int, host: str = "127.0.0.1"):
        self.fd = os.open(pty_path, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind((host, port))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        self.conn = None
        self._stop = False
        threading.Thread(target=self._accept, daemon=True).start()
        threading.Thread(target=self._pty_to_sock, daemon=True).start()

    def _accept(self) -> None:
        while not self._stop:
            try:
                conn, _ = self.srv.accept()
            except OSError:
                return
            old, self.conn = self.conn, conn
            if old is not None:
                old.close()
            threading.Thread(target=self._sock_to_pty, args=(conn,), daemon=True).start()

    def _sock_to_pty(self, conn) -> None:
        while not self._stop:
            try:
                data = conn.recv(4096)
            except OSError:
                return
            if not data:
                return
            os.write(self.fd, data)

    def _pty_to_sock(self) -> None:
        while not self._stop:
            r, _, _ = select.select([self.fd], [], [], 0.2)
            if not r:
                continue
            try:
                data = os.read(self.fd, 4096)
            except OSError:
                return
            conn = self.conn
            if conn is not None:
                try:
                    conn.sendall(data)
                except OSError:
                    pass

    def close(self) -> None:
        self._stop = True
        self.srv.close()
        if self.conn is not None:
            self.conn.close()
        os.close(self.fd)
