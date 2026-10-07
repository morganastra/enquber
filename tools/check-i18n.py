#!/usr/bin/env python3
"""Lint enquber's Qt ID-based translation setup.

The app passes only text IDs around: the C++ calls ``qtTrId("some.id")`` and
the English wording lives in a ``//%`` comment directly above the call, which
``lupdate`` copies into the catalogs as ``<source>``.  This script covers the
project conventions that no off-the-shelf tool does.  It fails when:

* source code still uses a text-based translation call (``tr()``,
  ``QCoreApplication::translate()``, the ``QT_TR*_NOOP`` family, ...);
* a ``qtTrId()`` call has no ``//%`` English comment directly above it;
* an ID is missing from a catalog, or a catalog carries an ID the source no
  longer uses;
* a catalog message has an empty ``<source>`` (so the app would show the raw
  ID at runtime);
* a widget is handed a user-facing string literal instead of a text ID.

Catalog fidelity (place markers, accelerators, surrounding whitespace, final
punctuation) is checked by Qt's own ``lcheck``, and completeness of the
translated catalogs by ``lrelease -fail-on-unfinished``; both run as separate
tests when the installed Qt provides the tools (Qt 6.11 and 6.10
respectively).  The ``//%``-comment and literal-string checks are heuristics
with a small allowlist.  The repository root is derived from this script's
location, so it runs from anywhere (for example as a ctest from the build
directory).
"""

from __future__ import annotations

import argparse
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

# ---------------------------------------------------------------------------
# Source scanning
# ---------------------------------------------------------------------------

CPP_SUFFIXES = (".cpp", ".cc", ".cxx", ".h", ".hpp", ".hxx")

TRANSLATION_CALL = re.compile(
    r"(?<![A-Za-z0-9_])"  # not part of a longer identifier (attr(, str(, ptr()
    r"(?:tr|translate)\s*\("
)
TRANSLATION_MACROS = re.compile(r"\bQT_TR(?:ANSLATE|ID)?(?:_N)?_NOOP[0-9]?(?:_UTF8)?\b")
QT_TRID = re.compile(r"\bqtTrId\s*\(\s*\"([^\"]+)\"")

# A C++ string literal, optionally wrapped in QStringLiteral(...).  The "text"
# group captures the literal's contents.
STRING_LITERAL = r"(?:QStringLiteral\s*\(\s*)?\"(?P<text>[^\"\\]*(?:\\.[^\"\\]*)*)\""

# Setters that take user-facing text.  A string literal here (as opposed to an
# ID, a variable, or an expression) is almost certainly a missed translation.
SETTER = re.compile(
    r"\b(?:setText|setWindowTitle|setToolTip|setAccessibleName|setAccessibleDescription"
    r"|setPlaceholderText|setStatusTip|setWhatsThis)\s*\(\s*" + STRING_LITERAL
)
WIDGET_WITH_TEXT = re.compile(
    r"\bnew\s+(?:QLabel|QPushButton|QAction|QCheckBox|QRadioButton|QToolButton)\s*\(\s*"
    + STRING_LITERAL
)

# Known, reviewed literals that are deliberately not translated.  Keep this
# list tiny and justified.
ALLOWED_LITERALS = {
    "?",  # the painted help/back glyph, a symbol rather than a word
}

# Region labels produced by label_regions().
CODE, COMMENT, LITERAL = "code", "comment", "literal"


def label_regions(text: str) -> list[str]:
    """Label each character of C++ source as CODE, COMMENT, or LITERAL.

    The labels line up with ``text`` one to one, so blanking a region never
    shifts a character and line numbers stay valid for diagnostics.  ``//``
    and ``/* */`` comments, escaped characters, and quotes inside comments
    (and vice versa) are all handled.
    """
    labels: list[str] = []
    i, n = 0, len(text)
    state = CODE  # CODE | COMMENT | LITERAL
    terminator = ""  # what ends the region: "\n" or "*/" for comments, the quote for literals
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == CODE:
            if c == "/" and nxt in ("/", "*"):
                state, terminator = COMMENT, ("\n" if nxt == "/" else "*/")
                labels.extend((COMMENT, COMMENT))
                i += 2
            elif c in "\"'":
                state, terminator = LITERAL, c
                labels.append(LITERAL)
                i += 1
            else:
                labels.append(CODE)
                i += 1
        elif state == COMMENT:
            if terminator == "\n":
                labels.append(COMMENT)
                if c == "\n":
                    state = CODE
                i += 1
            elif c == "*" and nxt == "/":
                labels.extend((COMMENT, COMMENT))
                state = CODE
                i += 2
            else:
                labels.append(COMMENT)
                i += 1
        else:  # LITERAL
            labels.append(LITERAL)
            if c == "\\" and nxt:  # escaped character, e.g. \" inside a string
                labels.append(LITERAL)
                i += 2
            else:
                if c == terminator:
                    state = CODE
                i += 1
    return labels


def mask(text: str, labels: list[str], *visible: str) -> str:
    """Return ``text`` with every character not labeled as ``visible`` blanked.

    Newlines are always kept, so the result has the same length and line
    structure as the input.
    """
    return "".join(
        c if c == "\n" or label in visible else " "
        for c, label in zip(text, labels, strict=True)
    )


@dataclass
class SourceReport:
    """What scan_sources found: the qtTrId IDs in use, and the violations."""

    ids: set[str] = field(default_factory=set)
    errors: list[str] = field(default_factory=list)


def english_comments_above(lines: list[str], lineno: int) -> list[str]:
    """The ``//%`` lines in the comment block directly above 1-based ``lineno``."""
    comments: list[str] = []
    for above in reversed(lines[: lineno - 1]):
        stripped = above.strip()
        if not stripped.startswith("//"):
            break
        if stripped.startswith("//%"):
            comments.append(stripped)
    return comments


def scan_sources(src_dir: Path) -> SourceReport:
    """Scan every C++ file under ``src_dir`` for violations and qtTrId IDs."""
    report = SourceReport()
    for path in sorted(src_dir.rglob("*")):
        if not path.is_file() or path.suffix not in CPP_SUFFIXES:
            continue
        raw = path.read_text(encoding="utf-8")
        labels = label_regions(raw)
        uncommented = mask(raw, labels, CODE, LITERAL)
        code_only = mask(raw, labels, CODE)
        rel = path.relative_to(src_dir)

        # Text-based translation calls anywhere in the code.
        for lineno, line in enumerate(code_only.split("\n"), start=1):
            if TRANSLATION_CALL.search(line) or TRANSLATION_MACROS.search(line):
                report.errors.append(
                    f'{rel}:{lineno}: text-based translation call; use qtTrId("id") instead'
                )

        # Every qtTrId needs exactly one English //% comment directly above it.
        # This catches a removed or duplicated //% without re-running lupdate;
        # the catalog-side empty-<source> check is the authoritative backstop.
        # The ID itself is a string literal, so match over the comment-masked
        # text and derive line numbers from match offsets.
        raw_lines = raw.split("\n")
        for match in QT_TRID.finditer(uncommented):
            report.ids.add(match.group(1))
            lineno = uncommented.count("\n", 0, match.start()) + 1
            comments = english_comments_above(raw_lines, lineno)
            if not comments:
                report.errors.append(
                    f"{rel}:{lineno}: qtTrId() has no //% English comment above it"
                )
            elif len(comments) > 1:
                report.errors.append(
                    f"{rel}:{lineno}: qtTrId() has {len(comments)} //% English "
                    f"comments above it; keep exactly one"
                )

        # User-facing string literals handed straight to widgets.
        for pattern in (SETTER, WIDGET_WITH_TEXT):
            for match in pattern.finditer(uncommented):
                lineno = uncommented.count("\n", 0, match.start()) + 1
                text = match.group("text")
                if text not in ALLOWED_LITERALS:
                    report.errors.append(
                        f"{rel}:{lineno}: user-facing string literal {text!r} "
                        f"passed to a widget; use a text ID"
                    )
    return report


# ---------------------------------------------------------------------------
# Catalog reading and ID sync
# ---------------------------------------------------------------------------


@dataclass
class Message:
    """One catalog entry: an ID and its English source, if any."""

    mid: str
    source: str


def read_catalog(path: Path) -> list[Message]:
    """Parse a .ts catalog, skipping messages that carry no id."""
    messages: list[Message] = []
    for element in ET.parse(path).getroot().iter("message"):
        mid = element.get("id")
        if not mid:
            continue
        source = element.find("source")
        messages.append(
            Message(
                mid=mid,
                source=(source.text or "") if source is not None else "",
            )
        )
    return messages


def diff_ids(catalog_name: str, catalog_ids: set[str], source_ids: set[str], errors: list[str]) -> None:
    """Report IDs found on only one side: missing from, or stale in, a catalog."""
    for mid in sorted(source_ids - catalog_ids):
        errors.append(f"{catalog_name}: id {mid!r} is missing (run lupdate; add //%)")
    for mid in sorted(catalog_ids - source_ids):
        errors.append(f"{catalog_name}: id {mid!r} is no longer used in the source")


def check_english_catalog(path: Path, source_ids: set[str], errors: list[str]) -> list[Message]:
    """Check the English source catalog and return its messages.

    It is the reference for every other catalog: each source ID must be in
    it, each entry must carry the engineering English, and it must not drift.
    """
    if not path.is_file():
        errors.append(f"missing English source catalog {path}")
        return []
    messages = read_catalog(path)
    diff_ids(path.name, {m.mid for m in messages}, source_ids, errors)
    for message in messages:
        if not message.source:
            errors.append(
                f"{path.name}: message {message.mid!r} has no <source>; "
                f"add or fix the //% English comment above qtTrId()"
            )
    return messages


def check_sync(path: Path, source_ids: set[str], errors: list[str]) -> None:
    """Check that one non-English catalog matches the source's IDs."""
    if not path.is_file():
        errors.append(f"missing catalog {path}")
        return
    diff_ids(path.name, {m.mid for m in read_catalog(path)}, source_ids, errors)


HELP_EPILOG = """\
checks:
  * no text-based translation call (tr(), translate(), QT_TR*_NOOP) left in
    the sources
  * every qtTrId() has a //% English comment directly above it
  * source IDs and catalog IDs are in sync, in both directions
  * no catalog message has an empty <source> (the //% comment is missing)
  * no user-facing string literal is handed straight to a widget

catalog fidelity and completeness are checked by Qt's lcheck and lrelease
(see the check_i18n_lcheck_* and check_i18n_translated_* ctests).

exit status is 1 when any check fails.
"""


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(
        description="Lint enquber's Qt ID-based translation setup (qtTrId IDs, "
        "//% comments, .ts catalog sync).",
        epilog=HELP_EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--source", type=Path, default=repo_root / "src", help="directory holding the C++ sources")
    parser.add_argument("--translations", type=Path, default=repo_root / "i18n", help="directory holding the .ts catalogs")
    parser.add_argument("--quiet", action="store_true", help="only print failures")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    errors: list[str] = []

    report = scan_sources(args.source)
    errors.extend(report.errors)
    if not report.ids:
        errors.append(f"no qtTrId() calls found under {args.source}")

    english = check_english_catalog(args.translations / "enquber_en.ts", report.ids, errors)
    for catalog in sorted(args.translations.glob("enquber_*.ts")):
        if catalog.name != "enquber_en.ts":
            check_sync(catalog, report.ids, errors)

    if errors:
        print("i18n check failed:", file=sys.stderr)
        for error in errors:
            print(f"  error: {error}", file=sys.stderr)
        return 1

    if not args.quiet:
        print(f"i18n check passed: {len(report.ids)} ids, {len(english)} English messages")
    return 0


if __name__ == "__main__":
    sys.exit(main())
