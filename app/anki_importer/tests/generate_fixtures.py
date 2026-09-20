#!/usr/bin/env python3
"""Generate deterministic APKG fixtures for the board-side importer tests."""

from __future__ import annotations

import argparse
import json
import shutil
import sqlite3
import struct
import tempfile
import zipfile
from pathlib import Path


ZIP_TIME = (2020, 1, 1, 0, 0, 0)


def zip_entry(name: str, data: bytes, compression: int) -> tuple[zipfile.ZipInfo, bytes]:
    info = zipfile.ZipInfo(name, ZIP_TIME)
    info.compress_type = compression
    info.create_system = 3
    info.external_attr = 0o100600 << 16
    return info, data


def write_zip(path: Path, entries: list[tuple[str, bytes, int]]) -> None:
    with zipfile.ZipFile(path, "w", allowZip64=True) as archive:
        for name, data, compression in entries:
            info, payload = zip_entry(name, data, compression)
            archive.writestr(info, payload)


def collection_bytes(path: Path, format_name: str) -> bytes:
    models = {
        "1000": {
            "id": 1000,
            "name": "Basic",
            "type": 0,
            "flds": [{"name": "Front"}, {"name": "Back"}],
            "tmpls": [
                {
                    "name": "Card 1",
                    "qfmt": "{{Front}}",
                    "afmt": "{{FrontSide}}<hr id=answer>{{Back}}",
                }
            ],
        },
        "2000": {
            "id": 2000,
            "name": "Cloze",
            "type": 1,
            "flds": [{"name": "Text"}, {"name": "Extra"}],
            "tmpls": [{"name": "Cloze", "qfmt": "{{cloze:Text}}", "afmt": "{{Extra}}"}],
        },
    }
    decks = {"1": {"id": 1, "name": "Fixture Deck"}}

    connection = sqlite3.connect(path)
    try:
        connection.execute("PRAGMA journal_mode=OFF")
        connection.execute("PRAGMA synchronous=OFF")
        connection.execute("PRAGMA page_size=4096")
        connection.execute("CREATE TABLE col (models TEXT NOT NULL, decks TEXT NOT NULL)")
        connection.execute(
            "CREATE TABLE notes (id INTEGER PRIMARY KEY, mid INTEGER NOT NULL, "
            "tags TEXT NOT NULL, flds TEXT NOT NULL)"
        )
        connection.execute(
            "CREATE TABLE cards (id INTEGER PRIMARY KEY, nid INTEGER NOT NULL, "
            "did INTEGER NOT NULL, ord INTEGER NOT NULL)"
        )
        connection.execute(
            "INSERT INTO col(models, decks) VALUES(?, ?)",
            (json.dumps(models, separators=(",", ":")),
             json.dumps(decks, separators=(",", ":"))),
        )

        if format_name == "anki20":
            front = (
                "Intro<div>Hello&nbsp;<b>World</b></div>"
                "<script>bad()</script><img src=\"pic.png\">"
                "<img src=\"missing.png\">"
            )
            back = "Answer &amp; detail [sound:audio.mp3] [latex]x[/latex]"
            connection.execute(
                "INSERT INTO notes VALUES(2001, 1000, ' tag1 tag2 ', ?)",
                (front + "\x1f" + back,),
            )
            connection.execute(
                "INSERT INTO notes VALUES(2002, 2000, '', ?)",
                ("{{c1::unsupported}}\x1fextra",),
            )
            connection.execute("INSERT INTO cards VALUES(3001, 2001, 1, 0)")
            connection.execute("INSERT INTO cards VALUES(3002, 2002, 1, 0)")
            connection.execute("INSERT INTO cards VALUES(3003, 2001, 1, 1)")
        else:
            connection.execute(
                "INSERT INTO notes VALUES(4001, 1000, '', ?)",
                ("2.1 front\x1f2.1 <b>back</b>",),
            )
            connection.execute("INSERT INTO cards VALUES(5001, 4001, 1, 0)")

        connection.commit()
        connection.execute("VACUUM")
    finally:
        connection.close()

    return path.read_bytes()


def mark_encrypted(path: Path) -> None:
    data = bytearray(path.read_bytes())
    for signature, flag_offset in ((b"PK\x03\x04", 6), (b"PK\x01\x02", 8)):
        offset = 0
        while True:
            offset = data.find(signature, offset)
            if offset < 0:
                break
            flags = struct.unpack_from("<H", data, offset + flag_offset)[0]
            struct.pack_into("<H", data, offset + flag_offset, flags | 1)
            offset += 4
    path.write_bytes(data)


def generate(output: Path) -> None:
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)

    with tempfile.TemporaryDirectory(prefix="moxinzhi-fixture-") as temp_dir:
        temp = Path(temp_dir)
        anki20 = collection_bytes(temp / "collection20.sqlite", "anki20")
        anki21 = collection_bytes(temp / "collection21.sqlite", "anki21")

    media_map = json.dumps(
        {"0": "pic.png", "1": "audio.mp3", "2": "unused.bin", "3": "missing.png"},
        separators=(",", ":"),
    ).encode()
    write_zip(
        output / "valid20.apkg",
        [
            ("collection.anki2", anki20, zipfile.ZIP_DEFLATED),
            ("media", media_map, zipfile.ZIP_DEFLATED),
            ("0", b"PNG-fixture", zipfile.ZIP_DEFLATED),
            ("1", b"AUDIO-fixture", zipfile.ZIP_STORED),
            ("2", b"UNUSED", zipfile.ZIP_STORED),
        ],
    )
    write_zip(
        output / "valid21.apkg",
        [
            ("collection.anki21", anki21, zipfile.ZIP_DEFLATED),
            ("media", b"{}", zipfile.ZIP_STORED),
        ],
    )
    write_zip(
        output / "corrupt-sqlite.apkg",
        [("collection.anki2", b"not sqlite", zipfile.ZIP_STORED)],
    )
    (output / "corrupt-zip.apkg").write_bytes(b"not a zip archive")
    write_zip(
        output / "traversal.apkg",
        [
            ("collection.anki2", anki20, zipfile.ZIP_STORED),
            ("../escape", b"blocked", zipfile.ZIP_STORED),
        ],
    )
    write_zip(
        output / "unsupported-compression.apkg",
        [("collection.anki2", anki20, zipfile.ZIP_BZIP2)],
    )
    write_zip(
        output / "encrypted.apkg",
        [("collection.anki2", anki20, zipfile.ZIP_STORED)],
    )
    mark_encrypted(output / "encrypted.apkg")
    write_zip(
        output / "compression-bomb.apkg",
        [("collection.anki2", b"A" * (256 * 1024), zipfile.ZIP_DEFLATED)],
    )
    write_zip(
        output / "oversized-entry.apkg",
        [("collection.anki2", b"X" * 4096, zipfile.ZIP_STORED)],
    )
    write_zip(
        output / "latest.apkg",
        [
            ("collection.anki2", b"dummy", zipfile.ZIP_STORED),
            ("collection.anki21b", b"\x28\xb5\x2f\xfdlatest", zipfile.ZIP_STORED),
            ("meta", b"\x08\x03", zipfile.ZIP_STORED),
        ],
    )

    expected = {
        "schema": "moxinzhi-anki-test-fixtures/1",
        "valid20": {
            "cards_imported": 1,
            "cards_skipped": 1,
            "media_imported": 2,
            "media_skipped": 2,
        },
        "valid21": {"cards_imported": 1, "media_imported": 0},
    }
    (output / "expected.json").write_text(
        json.dumps(expected, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    generate(args.output.resolve())


if __name__ == "__main__":
    main()
