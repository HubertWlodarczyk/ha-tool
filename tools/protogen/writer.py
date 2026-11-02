# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Small indentation-aware code writer with line wrapping for comments and calls."""

from __future__ import annotations

import textwrap
from collections.abc import Iterator
from contextlib import contextmanager


class Code:
    """Lines of generated code; `indent` spaces per level."""

    def __init__(self, indent: int = 4, width: int = 100):
        self.indent_size = indent
        self.width = width
        self.level = 0
        self.lines: list[str] = []

    @property
    def prefix(self) -> str:
        return " " * (self.indent_size * self.level)

    def line(self, text: str = "") -> None:
        self.lines.append(self.prefix + text if text else "")

    def raw(self, text: str) -> None:
        """Lines as given (already indented), e.g. from textwrap.dedent."""
        self.lines.extend(text.rstrip("\n").split("\n"))

    @contextmanager
    def indented(self) -> Iterator[None]:
        self.level += 1
        try:
            yield
        finally:
            self.level -= 1

    @contextmanager
    def block(self, head: str, tail: str = "}") -> Iterator[None]:
        """`head {` ... `}` (Java)."""
        self.line(head)
        with self.indented():
            yield
        self.line(tail)

    @contextmanager
    def method(self, head: str, params: list[str]) -> Iterator[None]:
        """`head(params) {` ... `}`, parameters wrapped one per line if too long (Java)."""
        self.call(head, params, " {")
        with self.indented():
            yield
        self.line("}")

    def wrapped(self, text: str, first: str = "", rest: str = "") -> list[str]:
        """text wrapped to the width, `first` / `rest` prepended to the lines."""
        avail = self.width - len(self.prefix) - max(len(first), len(rest))
        return [
            (first if i == 0 else rest) + part
            for i, part in enumerate(textwrap.wrap(text, max(avail, 20)) or [""])
        ]

    def javadoc(self, *paragraphs: str) -> None:
        """/** ... */ comment; paragraphs wrapped, the following ones start with <p>."""
        texts = [p.replace("*/", "*&#47;") for p in paragraphs if p]
        if len(texts) == 1 and len(self.prefix) + len(texts[0]) + 7 <= self.width:
            self.line(f"/** {texts[0]} */")
            return
        self.line("/**")
        for i, text in enumerate(texts):
            for part in self.wrapped(("<p>" if i else "") + text, " * ", " * "):
                self.line(part)
        self.line(" */")

    def call(self, head: str, args: list[str], tail: str, trailing_comma: bool = False) -> None:
        """head(arg, arg)tail on one line if it fits, otherwise one argument per line
        (trailing_comma: also after the last one, as Python formatters do)."""
        one = f"{head}({', '.join(args)}){tail}"
        if len(self.prefix) + len(one) <= self.width:
            self.line(one)
            return
        self.line(f"{head}(")
        with self.indented():
            for i, a in enumerate(args):
                last = i == len(args) - 1
                self.line(a + ("," if not last or trailing_comma else ""))
        self.line(f"){tail}")

    def text(self) -> str:
        return "\n".join(self.lines).rstrip("\n") + "\n"
