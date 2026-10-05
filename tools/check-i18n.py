#!/usr/bin/env python3
"""Lint enquber's Qt ID-based translation setup.

The app passes only text IDs around: source code calls ``qtTrId("some.id")``
and the English wording lives in a ``//%`` engineering-English comment that
``lupdate`` copies into the catalog as ``<source>``.  This script is the safety
net for that convention.  It fails when:

* a text-based translation call is still used in ``src/`` (``tr()``,
  ``QCoreApplication::translate()``, the ``QT_TR*_NOOP`` family, ...);
* an ID exists in the source but not in a catalog, or a catalog contains an ID
  the source no longer uses;
* a catalog message has an empty ``<source>`` (the ``//%`` comment is missing,
  which would make the app show the raw ID at runtime);
* a widget is given a user-facing string literal instead of a text ID;
* a catalog required by ``--require-translated`` is incomplete or breaks the
  source string's placeholders, accelerator, surrounding whitespace or final
  punctuation.

It derives the repository root from its own path, so it can run from anywhere
(for example as a ctest from the build directory).  It is a lint, not a proof:
the "no literal strings" checks use heuristics and a small allowlist.
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
    r"(?<![A-Za-z0-9_])"  # not part of a longer identifier (attr/, str/, ptr/)
    r"(?:tr|translate)\s*\("
)
TRANSLATION_MACROS = re.compile(r"QT_TR(?:ID|LATE)?(?:_N)?_NOOP[0-9]?\b")
QT_TRID = re.compile(r"\bqtTrId\s*\(\s*\"([^\"]+)\"")

# Setters that take user-facing text.  A string literal here (as opposed to an
# ID, a variable, or an expression) is almost certainly a missed translation.
SETTER = re.compile(
    r"\b(?:setText|setWindowTitle|setToolTip|setAccessibleName|setAccessibleDescription"
    r"|setPlaceholderText|setStatusTip|setWhatsThis)\s*\(\s*(?P<value>QStringLiteral\s*\(\s*)?\"(?P<text>[^\"\\]*(?:\\.[^\"\\]*)*)\""
)
WIDGET_WITH_TEXT = re.compile(
    r"\bnew\s+(?:QLabel|QPushButton|QAction|QCheckBox|QRadioButton|QToolButton)\s*\(\s*"
    r"(?P<value>QStringLiteral\s*\(\s*)?\"(?P<text>[^\"\\]*(?:\\.[^\"\\]*)*)\""
)

# Known, reviewed literals that are deliberately not translated.  Match on the
# literal text.  Keep this list tiny and justified.
ALLOWED_LITERALS = {
    "?",  # the painted help/back glyph, a symbol rather than a word
}


def strip_comments(text: str) -> str:
    """Replace // and /* */ comments with spaces, keeping strings intact.

    Line structure is preserved so nothing else about the file has to change.
    """
    out: list[str] = []
    i = 0
    n = len(text)
    state = "code"  # code | line | block | string | char
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                state = "line"
                out.append("  ")
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "block"
                out.append("  ")
                i += 2
                continue
            if c == '"':
                state = "string"
            elif c == "'":
                state = "char"
            out.append(c)
            i += 1
        elif state == "line":
            if c == "\n":
                state = "code"
                out.append(c)
            else:
                out.append(" ")
            i += 1
        elif state == "block":
            if c == "*" and nxt == "/":
                state = "code"
                out.append("  ")
                i += 2
                continue
            out.append("\n" if c == "\n" else " ")
            i += 1
        elif state in ("string", "char"):
            out.append(c)
            if c == "\\":  # escaped character
                if i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
            elif (state == "string" and c == '"') or (state == "char" and c == "'"):
                state = "code"
            i += 1
    return "".join(out)


def blank_strings(text: str) -> str:
    """Replace the contents of string/char literals with spaces."""
    out: list[str] = []
    i = 0
    n = len(text)
    state = "code"
    while i < n:
        c = text[i]
        if state == "code":
            if c == '"':
                state = "string"
                out.append(c)
            elif c == "'":
                state = "char"
                out.append(c)
            else:
                out.append(c)
            i += 1
        elif state == "string":
            if c == "\\":
                out.append("  ")
                i += 2
                continue
            if c == '"':
                state = "code"
                out.append(c)
            else:
                out.append("\n" if c == "\n" else " ")
            i += 1
        else:  # char
            if c == "\\":
                out.append("  ")
                i += 2
                continue
            if c == "'":
                state = "code"
                out.append(c)
            else:
                out.append("\n" if c == "\n" else " ")
            i += 1
    return "".join(out)


@dataclass
class SourceReport:
    ids: list[str] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    @property
    def id_set(self) -> set[str]:
        return set(self.ids)


def scan_sources(src_dir: Path) -> SourceReport:
    report = SourceReport()
    for path in sorted(src_dir.rglob("*")):
        if not path.is_file() or path.suffix not in CPP_SUFFIXES:
            continue
        raw = path.read_text(encoding="utf-8")
        uncommented = strip_comments(raw)
        code_only = blank_strings(uncommented)
        rel = path.relative_to(src_dir)

        for lineno, line in enumerate(code_only.splitlines(), start=1):
            if TRANSLATION_CALL.search(line) or TRANSLATION_MACROS.search(line):
                report.errors.append(
                    f"{rel}:{lineno}: text-based translation call; use qtTrId(\"id\") instead"
                )

        for match in QT_TRID.finditer(uncommented):
            report.ids.append(match.group(1))

        # Every qtTrId needs its engineering-English //% comment directly above
        # it.  This catches a removed //% without having to re-run lupdate; the
        # catalog-side empty-<source> check is the authoritative backstop.
        raw_lines = raw.splitlines()
        for lineno, line in enumerate(raw_lines, start=1):
            if not QT_TRID.search(line):
                continue
            has_engineering_english = False
            cursor = lineno - 2  # previous line, 0-based
            while cursor >= 0:
                previous = raw_lines[cursor].strip()
                if previous.startswith("//"):
                    if previous.startswith("//%"):
                        has_engineering_english = True
                    cursor -= 1
                    continue
                break
            if not has_engineering_english:
                report.errors.append(
                    f"{rel}:{lineno}: qtTrId() has no //% engineering-English comment above it"
                )

        for lineno, line in enumerate(uncommented.splitlines(), start=1):
            for pattern in (SETTER, WIDGET_WITH_TEXT):
                for match in pattern.finditer(line):
                    text = match.group("text")
                    if text in ALLOWED_LITERALS:
                        continue
                    report.errors.append(
                        f"{rel}:{lineno}: user-facing string literal {text!r} "
                        f"passed to a widget; use a text ID"
                    )
    return report


# ---------------------------------------------------------------------------
# Catalog parsing and validation
# ---------------------------------------------------------------------------

PLACEHOLDER = re.compile(r"%(?:\d+|n|L\d+)")
DIGIT_PERCENT = re.compile(r"%\d+")
PUNCTUATION = ".,;:!?…"


@dataclass
class Message:
    mid: str
    source: str
    translation: str
    unfinished: bool
    vanished: bool


def unescape_ts(text: str | None) -> str:
    if text is None:
        return ""
    # XML already resolved the entities; lupdate also escapes \n and \" in TS.
    return text.replace("\n", "\n")


def read_catalog(path: Path) -> list[Message]:
    tree = ET.parse(path)
    root = tree.getroot()
    messages: list[Message] = []
    for context in root.findall("context"):
        for message in context.findall("message"):
            mid = message.get("id")
            if not mid:
                continue
            source_el = message.find("source")
            source = source_el.text if source_el is not None and source_el.text else ""
            translation_el = message.find("translation")
            translation = ""
            unfinished = False
            vanished = False
            if translation_el is not None:
                unfinished = translation_el.get("type") == "unfinished"
                vanished = translation_el.get("type") == "vanished"
                translation = "".join(translation_el.itertext())
            messages.append(Message(mid, source, translation, unfinished, vanished))
    return messages


def compare_strings(mid: str, source: str, translation: str) -> list[str]:
    """Return the ways a translation violates its source string."""
    problems: list[str] = []
    src_ph = sorted(PLACEHOLDER.findall(source))
    tr_ph = sorted(PLACEHOLDER.findall(translation))
    if src_ph != tr_ph:
        problems.append(f"placeholders differ: source {src_ph}, translation {tr_ph}")

    # A % followed by a digit that is not a real placeholder is eaten by arg().
    valid = set(src_ph)
    for marker in DIGIT_PERCENT.findall(translation):
        if marker not in valid:
            problems.append(f"literal {marker!r} would be consumed by QString::arg()")
            break

    if single_accelerator(source) != single_accelerator(translation):
        problems.append("accelerator (&) count differs from the source")

    src_lead = source[: len(source) - len(source.lstrip())]
    tr_lead = translation[: len(translation) - len(translation.lstrip())]
    if src_lead != tr_lead:
        problems.append(f"leading whitespace differs (source {src_lead!r})")
    src_trail = source[len(source.rstrip()) :]
    tr_trail = translation[len(translation.rstrip()) :]
    if src_trail != tr_trail:
        problems.append(f"trailing whitespace differs (source {src_trail!r})")

    if source and translation and source[-1] in PUNCTUATION and translation[-1] != source[-1]:
        problems.append(f"final punctuation {translation[-1]!r} != source {source[-1]!r}")
    return problems


def single_accelerator(text: str) -> int:
    return text.replace("&&", "").count("&")


def parse_language(value: str) -> tuple[str, Path]:
    """Accept a language code (es) or an explicit .ts path (i18n/foo_es.ts)."""
    if value.endswith(".ts"):
        path = Path(value)
        stem = path.stem
        lang = stem.split("_")[-1] if "_" in stem else stem
        return lang, path
    return value, Path(f"enquber_{value}.ts")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    here = Path(__file__).resolve().parent.parent
    parser.add_argument("--source", type=Path, default=here / "src", help="directory holding the C++ sources")
    parser.add_argument("--translations", type=Path, default=here / "i18n", help="directory holding the .ts catalogs")
    parser.add_argument(
        "--require-translated",
        action="append",
        default=[],
        metavar="LANG",
        help="catalog that must be fully translated (language code or .ts path); repeatable",
    )
    parser.add_argument("--quiet", action="store_true", help="only print failures")
    args = parser.parse_args(argv)

    errors: list[str] = []
    warnings: list[str] = []

    source = scan_sources(args.source)
    errors.extend(source.errors)
    warnings.extend(source.warnings)

    if not source.ids:
        errors.append(f"no qtTrId() calls found under {args.source}")

    # The English source catalog is the reference: every source ID must be in
    # it, every entry must carry engineering English, and it must not drift.
    english_path = args.translations / "enquber_en.ts"
    if not english_path.is_file():
        errors.append(f"missing English source catalog {english_path}")
        english: list[Message] = []
    else:
        english = read_catalog(english_path)
        english_ids = {m.mid for m in english}
        for mid in sorted(source.id_set - english_ids):
            errors.append(f"{english_path.name}: source id {mid!r} is missing (run lupdate; add //%)")
        for mid in sorted(english_ids - source.id_set):
            errors.append(f"{english_path.name}: id {mid!r} is no longer used in the source")
        for message in english:
            if not message.source:
                errors.append(
                    f"{english_path.name}: message {message.mid!r} has no <source>; "
                    f"add a //% engineering-English comment above qtTrId()"
                )

    english_sources = {m.mid: m.source for m in english}

    for value in args.require_translated:
        lang, rel = parse_language(value)
        path = rel if rel.is_absolute() else args.translations / rel.name
        if not path.is_file():
            errors.append(f"required catalog {path} not found")
            continue
        messages = read_catalog(path)
        by_id = {m.mid: m for m in messages}
        for mid in sorted(source.id_set - by_id.keys()):
            errors.append(f"{path.name}: id {mid!r} is missing")
        for mid in sorted(by_id.keys() - source.id_set):
            errors.append(f"{path.name}: id {mid!r} is no longer used in the source")
        for message in messages:
            if message.vanished:
                continue
            if message.unfinished or not message.translation:
                errors.append(f"{path.name}: {message.mid!r} is not translated")
                continue
            reference = english_sources.get(message.mid)
            if reference is None:
                continue
            for problem in compare_strings(message.mid, reference, message.translation):
                errors.append(f"{path.name}: {message.mid!r}: {problem}")

    if errors:
        print("i18n check failed:", file=sys.stderr)
        for error in errors:
            print(f"  error: {error}", file=sys.stderr)
        for warning in warnings:
            print(f"  warning: {warning}", file=sys.stderr)
        return 1

    if not args.quiet:
        print(f"i18n check passed: {len(source.ids)} ids, {len(english)} English messages")
        for warning in warnings:
            print(f"  warning: {warning}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
