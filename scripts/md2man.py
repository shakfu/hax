#!/usr/bin/env python3
"""Render the user docs in docs/ as man pages.

Usage: scripts/md2man.py VERSION OUTPUT...

Each OUTPUT is a path whose file name selects a page from PAGES (hax.1, hax-config.5, ...); the
page is written there. The build passes every page, so naming an unknown page or omitting one
fails rather than letting meson's output list and PAGES drift apart.

The Markdown stays the single source, readable on GitHub. A document's H1 is its title and is
dropped; H2 becomes a man section and H3 a subsection. Text before the first H2 lands under
DESCRIPTION. A line `<!-- include PATH -->` splices in another document without its title, which
is how hax.1 wraps docs/usage.md in the conventional man sections.

Only the Markdown subset the docs use is understood: paragraphs, ATX headings, fenced code,
flat bullet and numbered lists, pipe tables, code spans, emphasis, links, and hard line breaks.
Anything else fails the build instead of rendering wrong, as does a link to a document no page
covers. Tables render as tagged paragraphs: the first column is the tag, the last column the
text, and any middle columns follow as "Header: value." lines, since 80-column man output
cannot fit the docs' wide tables. Code blocks are not wrapped, so a code line too long for an
80-column terminal fails the build too.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path
from typing import NamedTuple

ROOT = Path(__file__).resolve().parent.parent


class Page(NamedTuple):
    name: str
    section: int
    source: str
    summary: str

    @property
    def filename(self) -> str:
        return f"{self.name}.{self.section}"


PAGES = (
    Page("hax", 1, "docs/man/hax.1.md", "minimalist, terminal-native coding agent"),
    Page("hax-config", 5, "docs/configuration.md", "hax configuration file and settings"),
    Page("hax-sessions", 5, "docs/sessions.md", "hax session files and JSON output"),
    Page("hax-providers", 7, "docs/providers.md", "model providers for hax"),
)

INCLUDE_RE = re.compile(r"^<!-- include (\S+) -->$")
HEADING_RE = re.compile(r"^(#{1,6}) +(.*?)#*\s*$")
FENCE_RE = re.compile(r"^```")
BULLET_RE = re.compile(r"^- +")
NUMBER_RE = re.compile(r"^(\d+)\. +")
RAW_HTML_RE = re.compile(r"^<(?!(https?|mailto):)")
# Blocks this script does not render. CommonMark lets each of them start without a blank line
# before it, so a paragraph must end at one rather than absorb it as text.
UNSUPPORTED_BLOCK_RE = re.compile(
    r"^ {0,3}(?:"
    r">"  # block quote, including GitHub alerts
    r"|[*+](?:\s|$)|\d+\)(?:\s|$)"  # list markers other than "-" and "1."
    r"|=+\s*$|-+\s*$"  # setext heading underline
    r"|(?P<rule>[-*_])(?:\s*(?P=rule)){2,}\s*$"  # thematic break
    r")"
)
TABLE_DELIMITER_RE = re.compile(r"^\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)*\|?$")
PUNCTUATION = set("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~")
# Tables lay out for an 80-column terminal. When most tags are at most TAG_BESIDE_MAX wide, they
# set a column with the text beside them, and a longer tag takes a line of its own. A table whose
# tags all fit and whose texts fit in about two lines drops the blank line between rows.
TAG_BESIDE_MAX = 16
COMPACT_TEXT_MAX = 100
DEFAULT_INDENT = 7
# Code is not wrapped. mandoc indents it 7 + 4 columns, leaving this much of an 80-column line.
CODE_WIDTH_MAX = 69
HARD_BREAK = "\ue000"  # private-use stand-in for a trailing-backslash line break


class DocError(Exception):
    pass


def fail_at(path: Path, lineno: int, message: str) -> DocError:
    return DocError(f"{path.relative_to(ROOT)}:{lineno}: {message}")


class Source:
    """One document being rendered, tracked for diagnostics and relative links."""

    def __init__(self, path: Path, page: Page, pages_by_source: dict[Path, Page]) -> None:
        self.path = path
        self.page = page
        self.pages_by_source = pages_by_source
        self.lineno = 0

    def error(self, message: str) -> DocError:
        return fail_at(self.path, self.lineno, message)


# --- inline ---


def escape(text: str, literal: bool = False, upper: bool = False) -> str:
    """Escape text for roff. Literal text (code, URLs) keeps ASCII hyphens and quotes, which
    groff would otherwise typeset as Unicode lookalikes that break copy and search."""
    out = []
    prev = ""
    for ch in text:
        if upper:
            ch = ch.upper()
        if ch == "\\":
            out.append("\\e")
        elif ch == "-":
            # In prose only a hyphen inside a word is typographic; one that starts a token is
            # an option dash.
            out.append("\\-" if literal or not prev.isalnum() else "-")
        elif ch == "'" and literal:
            out.append("\\(aq")
        elif ch == "`":
            out.append("\\(ga")
        elif ch == "~":
            out.append("\\(ti")
        elif ch == "^":
            out.append("\\(ha")
        elif ch == HARD_BREAK:
            out.append(f"\n{HARD_BREAK}\n")
        elif ord(ch) > 127:
            out.append(f"\\[u{ord(ch):04X}]")
        else:
            out.append(ch)
        prev = ch
    return "".join(out)


def page_ref(page: Page) -> str:
    # \% keeps the line from breaking at the hyphen inside a name like hax-config.
    return f"\\fB\\%{escape(page.name)}\\fP({page.section})"


def render_link(src: Source, label: str, target: str, upper: bool) -> str:
    text = render_inline(src, label, upper)
    if re.match(r"^(https?|mailto):", target):
        url = escape(target, literal=True)
        return url if label == target else f"{text} <{url}>"
    path_part, _, _ = target.partition("#")
    if not path_part:
        return text
    resolved = Path(os.path.normpath(src.path.parent / path_part))
    page = src.pages_by_source.get(resolved)
    if page is None:
        raise src.error(f"link to {path_part}, which no man page covers")
    if page == src.page:
        return text
    bare_label = re.sub(r"[`*]", "", label)
    if bare_label.endswith(".md") or bare_label == f"{page.name}({page.section})":
        return page_ref(page)
    return f"{text} ({page_ref(page)})"


def find_closing(text: str, start: int, delimiter: str) -> int:
    end = text.find(delimiter, start)
    while end != -1 and text[end - 1] == "\\":
        end = text.find(delimiter, end + 1)
    return end


def render_inline(src: Source, text: str, upper: bool = False) -> str:
    out = []
    plain = []

    def flush() -> None:
        out.append(escape("".join(plain), upper=upper))
        plain.clear()

    i = 0
    while i < len(text):
        ch = text[i]
        if ch == "\\" and i + 1 < len(text) and text[i + 1] in PUNCTUATION:
            plain.append(text[i + 1])
            i += 2
            continue
        if ch == "`":
            run = len(text) - i - len(text[i:].lstrip("`"))
            fence = "`" * run
            end = text.find(fence, i + run)
            while end != -1 and text[end + run : end + run + 1] == "`":
                end = text.find(fence, end + run + 1)
            if end == -1:
                raise src.error("unterminated code span")
            code = text[i + run : end].replace("\n", " ")
            if len(code) > 2 and code[0] == " " and code[-1] == " " and code.strip():
                code = code[1:-1]
            flush()
            out.append(f"\\fB{escape(code, literal=True)}\\fP")
            i = end + run
            continue
        if text.startswith("**", i):
            end = find_closing(text, i + 2, "**")
            if end == -1:
                raise src.error("unterminated **strong** span")
            flush()
            out.append(f"\\fB{render_inline(src, text[i + 2 : end], upper)}\\fP")
            i = end + 2
            continue
        if ch == "*" and i + 1 < len(text) and not text[i + 1].isspace():
            end = find_closing(text, i + 1, "*")
            if end == -1:
                raise src.error("unterminated *emphasis* span")
            flush()
            out.append(f"\\fI{render_inline(src, text[i + 1 : end], upper)}\\fP")
            i = end + 1
            continue
        if ch == "[":
            match = re.compile(r"\[((?:[^\]\\]|\\.)*)\]\(([^)\s]+)\)").match(text, i)
            if match:
                flush()
                out.append(render_link(src, match.group(1), match.group(2), upper))
                i = match.end()
                continue
        if ch == "<":
            match = re.compile(r"<((?:https?|mailto):[^>\s]+)>").match(text, i)
            if match:
                flush()
                out.append(escape(match.group(1), literal=True))
                i = match.end()
                continue
        if ch == "!" and text.startswith("![", i):
            raise src.error("images are not supported")
        plain.append(ch)
        i += 1
    flush()
    return "".join(out)


def text_lines(rendered: str) -> list[str]:
    """Split rendered text into roff input lines. Leading spaces would force a break and a
    leading dot or quote would start a request, so neither survives."""
    lines = []
    for line in rendered.split("\n"):
        line = line.strip()
        if not line:
            continue
        if line == HARD_BREAK:
            line = ".br"
        elif line[0] in ".'":
            line = "\\&" + line
        lines.append(line)
    return lines


def render_text(src: Source, lines: list[str]) -> list[str]:
    """Render a paragraph as one input line. roff puts two spaces after a sentence that ends an
    input line, so keeping the Markdown's line breaks would space sentences unevenly."""
    joined = []
    for index, line in enumerate(lines):
        if line.endswith("\\") and index < len(lines) - 1:
            line = line[:-1] + HARD_BREAK
        joined.append(line.strip())
    return text_lines(render_inline(src, " ".join(joined)))


# --- blocks ---


def split_row(src: Source, line: str) -> list[str]:
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|") and not line.endswith("\\|"):
        line = line[:-1]
    cells = []
    cell = []
    i = 0
    in_code = False
    while i < len(line):
        ch = line[i]
        if ch == "\\" and i + 1 < len(line) and line[i + 1] == "|":
            cell.append("|")
            i += 2
            continue
        if ch == "`":
            in_code = not in_code
        if ch == "|" and not in_code:
            cells.append("".join(cell).strip())
            cell = []
        else:
            cell.append(ch)
        i += 1
    cells.append("".join(cell).strip())
    if in_code:
        raise src.error("unterminated code span in table row")
    return cells


def visible_width(markdown: str) -> int:
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", markdown)
    return len(re.sub(r"[`*\\]", "", text))


def render_table(src: Source, rows: list[str]) -> list[str]:
    if len(rows) < 3 or not TABLE_DELIMITER_RE.match(rows[1].strip()):
        raise src.error("table needs a header row, a delimiter row, and a body")
    header = split_row(src, rows[0])
    body = [split_row(src, row) for row in rows[2:]]
    for cells in body:
        if len(cells) != len(header):
            raise src.error(f"table row has {len(cells)} cells, header has {len(header)}")

    widths = [visible_width(cells[0]) for cells in body]
    fitting = [width for width in widths if width <= TAG_BESIDE_MAX]
    beside = 2 * len(fitting) >= len(widths)
    indent = max(fitting) + 2 if beside else DEFAULT_INDENT
    compact = (
        len(fitting) == len(widths)
        and len(header) == 2
        and all(visible_width(cells[-1]) <= COMPACT_TEXT_MAX for cells in body)
    )

    out = []
    for index, cells in enumerate(body):
        out += [f".TP {indent}n", *text_lines(render_inline(src, cells[0]))]
        # The first row keeps its space from the text above.
        if compact and index == 0:
            out.append(".PD 0")
        out += text_lines(render_inline(src, cells[-1]))
        extras = []
        for label, value in zip(header[1:-1], cells[1:-1]):
            extras.append(f"{label}: {value.rstrip('.')}.")
        if extras:
            out.append(".br")
            out += text_lines(render_inline(src, " ".join(extras)))
    if compact:
        out.append(".PD")
    return out


def render_code(lines: list[str]) -> list[str]:
    body = []
    for line in lines:
        escaped = escape(line, literal=True)
        body.append("\\&" + escaped if escaped[:1] in (".", "'") else escaped)
    return [".PP", ".RS 4", ".EX", *body, ".EE", ".RE"]


def is_block_start(line: str) -> bool:
    return bool(
        not line.strip()
        or HEADING_RE.match(line)
        or FENCE_RE.match(line)
        or BULLET_RE.match(line)
        or NUMBER_RE.match(line)
        or line.startswith("|")
        or RAW_HTML_RE.match(line)
        or UNSUPPORTED_BLOCK_RE.match(line)
    )


def render_document(src: Source) -> list[str]:
    lines = src.path.read_text(encoding="utf-8").splitlines()
    out: list[str] = []
    title_seen = False
    i = 0

    def at(index: int) -> str:
        src.lineno = index + 1
        return lines[index]

    while i < len(lines):
        line = at(i)
        if not line.strip():
            i += 1
            continue

        if (match := INCLUDE_RE.match(line)) is not None:
            included = Path(os.path.normpath(src.path.parent / match.group(1)))
            if src.pages_by_source.get(included) != src.page:
                raise src.error(f"include of {match.group(1)}, which is not part of this page")
            out += render_document(Source(included, src.page, src.pages_by_source))
            i += 1
            continue

        if (match := HEADING_RE.match(line)) is not None:
            level = len(match.group(1))
            if level == 1:
                if title_seen:
                    raise src.error("a second H1; only the title may be level 1")
                title_seen = True
            elif level == 2:
                out += [".SH", *text_lines(render_inline(src, match.group(2), upper=True))]
            elif level == 3:
                out += [".SS", *text_lines(render_inline(src, match.group(2)))]
            else:
                raise src.error("headings deeper than H3 are not supported")
            i += 1
            continue

        if FENCE_RE.match(line):
            start = i
            i += 1
            while i < len(lines) and not FENCE_RE.match(at(i)):
                i += 1
            if i == len(lines):
                src.lineno = start + 1
                raise src.error("unterminated code fence")
            for index in range(start + 1, i):
                if len(lines[index]) > CODE_WIDTH_MAX:
                    src.lineno = index + 1
                    raise src.error(f"code line longer than {CODE_WIDTH_MAX} columns")
            out += render_code(lines[start + 1 : i])
            i += 1
            continue

        if RAW_HTML_RE.match(line) or UNSUPPORTED_BLOCK_RE.match(line) or line.startswith("    "):
            raise src.error(
                "raw HTML, block quotes, rules, setext headings, indented code, and lists marked "
                "with *, +, or 1) are not supported"
            )

        if line.startswith("|"):
            start = i
            while i < len(lines) and at(i).startswith("|"):
                i += 1
            src.lineno = start + 1
            out += render_table(src, lines[start:i])
            continue

        if BULLET_RE.match(line) or NUMBER_RE.match(line):
            first_item = True
            while i < len(lines):
                item = at(i)
                if (match := BULLET_RE.match(item)) is not None:
                    out.append(".IP \\(bu 2")
                elif (match := NUMBER_RE.match(item)) is not None:
                    out.append(f".IP {match.group(1)}. 4")
                else:
                    break
                # Items sit flush together, but the first keeps its space from the text above.
                if first_item:
                    out.append(".PD 0")
                    first_item = False
                body = [item[match.end() :]]
                i += 1
                while i < len(lines) and not is_block_start(at(i)):
                    if re.match(r"^\s+(- |\d+\. )", lines[i]):
                        raise src.error("nested lists are not supported")
                    body.append(lines[i])
                    i += 1
                out += render_text(src, body)
                if i < len(lines) and not lines[i].strip():
                    following = i + 1
                    if following < len(lines) and lines[following].startswith(" "):
                        src.lineno = following + 1
                        raise src.error("multi-paragraph list items are not supported")
            out.append(".PD")
            continue

        start = i
        while i < len(lines) and (i == start or not is_block_start(at(i))):
            i += 1
        out += [".PP", *render_text(src, lines[start:i])]

    return out


def release_date(version: str) -> str:
    """The version's date from CHANGELOG.md, so a page's footer is reproducible. Builds between
    releases still carry the last released version, and with it that release's date; a version
    without a dated section, such as a release candidate, gets no date."""
    changelog = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    heading = rf"^## \[{re.escape(version)}\] - (\d{{4}}-\d{{2}}-\d{{2}})$"
    match = re.search(heading, changelog, re.MULTILINE)
    return match.group(1) if match else ""


def drop_redundant_paragraphs(lines: list[str]) -> list[str]:
    """A heading already starts a paragraph; mandoc warns about a .PP right after one."""
    out: list[str] = []
    for line in lines:
        after_heading = (len(out) >= 2 and out[-2] in (".SH", ".SS")) or out[-1:] == [
            ".SH DESCRIPTION"
        ]
        if line == ".PP" and after_heading:
            continue
        out.append(line)
    return out


def render_page(page: Page, version: str, date: str, pages_by_source: dict[Path, Page]) -> str:
    src = Source(ROOT / page.source, page, pages_by_source)
    body = render_document(src)
    head = [
        f'.TH "{page.name.upper()}" "{page.section}" "{date}" "hax {version}" "hax manual"',
        # groff's man macros restore hyphenation (HY) and adjustment (AD) at every paragraph, so
        # .nh and .ad alone last only until the first one. Identifiers split across lines are
        # hard to search for, and justifying lines full of them leaves wide gaps.
        ".nr HY 0",
        ".ds AD l",
        ".nh",
        ".ad l",
        ".SH NAME",
        f"{escape(page.name)} \\- {escape(page.summary)}",
    ]
    # Text before the first section heading reads as the page's description.
    if body and body[0] != ".SH":
        head.append(".SH DESCRIPTION")
    return "\n".join(drop_redundant_paragraphs(head + body)) + "\n"


def collect_sources() -> dict[Path, Page]:
    """Map every rendered document, including spliced-in ones, to its page."""
    pages_by_source = {}
    for page in PAGES:
        path = ROOT / page.source
        pages_by_source[path] = page
        for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if (match := INCLUDE_RE.match(line)) is not None:
                included = Path(os.path.normpath(path.parent / match.group(1)))
                if not included.is_file():
                    raise fail_at(path, lineno, f"included {match.group(1)} does not exist")
                pages_by_source[included] = page
    return pages_by_source


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: scripts/md2man.py VERSION OUTPUT...", file=sys.stderr)
        return 2
    version, outputs = sys.argv[1], [Path(arg) for arg in sys.argv[2:]]
    by_filename = {page.filename: page for page in PAGES}
    requested = sorted(output.name for output in outputs)
    if requested != sorted(by_filename):
        print(
            f"md2man.py: outputs {requested} do not match PAGES {sorted(by_filename)}",
            file=sys.stderr,
        )
        return 2
    try:
        date = release_date(version)
        pages_by_source = collect_sources()
        for output in outputs:
            rendered = render_page(by_filename[output.name], version, date, pages_by_source)
            output.write_text(rendered, encoding="utf-8")
    except DocError as error:
        print(f"md2man.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
