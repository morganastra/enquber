# Translations

Enquber uses Qt's ID-based translations. Use `qtTrId()` with IDs,
not `tr()` with strings. English wording goes in a `//%` comment above
the call. 

When you add or change a user-facing string:

1. Call `qtTrId("component.element")` and put the English text in a `//%`
   comment on the line directly above it. Use exactly one `//%` per call, and
   add a `//:` note when the context is not obvious to a translator.
2. Run `just i18n-update` to add the ID to every `i18n/enquber_*.ts`.
3. Translate the new entries in Qt Linguist (`linguist6`) when you can. 
4. Run `just test`. The `check_i18n` lint fails on a missing `//%`, a stale
   catalog or a leftover `tr()`; on Qt 6.11+ Qt's `lcheck` checks
   placeholders, accelerators, surrounding whitespace and final punctuation.

Spanish ships in `i18n/enquber_es.ts`. To try it:

    LANGUAGE=es ./build/enquber "https://example.com"

Localization happens at startup; there is no language switch in the running
app, and right-to-left mirroring is not implemented yet.

