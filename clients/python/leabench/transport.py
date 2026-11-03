# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Transports to a board: pty or termios serial device, pyserial, TCP (simulation bridge)."""

from __future__ import annotations

import os
import select
import socket


class Transport:
    """Byte stream to a board."""

    def read(self, timeout: float) -> bytes:
        """Bytes received within timeout s (empty if none); OSError when the link is gone."""
        raise NotImplementedError

    def write(self, data: bytes) -> None:
        raise NotImplementedError

    def close(self) -> None:
        raise NotImplementedError


class FdTransport(Transport):
    """A pty (simulation) or a serial device configured with termios (Linux, macOS)."""

    def __init__(self, path: str, baud: int | None):
        import termios
        import tty

        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)
        if baud:
            attrs = termios.tcgetattr(self.fd)
            attrs[4] = attrs[5] = getattr(termios, f"B{baud}")
            attrs[2] &= ~termios.CRTSCTS
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
            termios.tcflush(self.fd, termios.TCIOFLUSH)

    def read(self, timeout: float) -> bytes:
        r, _, _ = select.select([self.fd], [], [], timeout)
        return os.read(self.fd, 4096) if r else b""

    def write(self, data: bytes) -> None:
        os.write(self.fd, data)

    def close(self) -> None:
        os.close(self.fd)


class SerialTransport(Transport):
    """pyserial: any OS (COM3 on Windows)."""

    def __init__(self, port: str, baud: int):
        import serial  # pyserial

        self.ser = serial.Serial(port, baud, timeout=0.1, rtscts=False)

    def read(self, timeout: float) -> bytes:
        self.ser.timeout = timeout
        return self.ser.read(self.ser.in_waiting or 1)

    def write(self, data: bytes) -> None:
        self.ser.write(data)

    def close(self) -> None:
        self.ser.close()


class TcpTransport(Transport):
    """The simulation's TCP bridge (scripts/sim.py --tcp)."""

    def __init__(self, host: str, port: int):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.settimeout(None)

    def read(self, timeout: float) -> bytes:
        r, _, _ = select.select([self.sock], [], [], timeout)
        if not r:
            return b""
        data = self.sock.recv(4096)
        if not data:
            raise ConnectionError("connection closed")
        return data

    def write(self, data: bytes) -> None:
        self.sock.sendall(data)

    def close(self) -> None:
        self.sock.close()
