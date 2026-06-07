"""Register a converted pack's color schemes into the Sidekick toolkit database.

It runs under Unreal's bundled standalone Python, launched by the plugin's module at editor
startup, before the Sidekick toolkit opens the database. Once the toolkit opens it, it holds
the connection for the rest of the session and refuses every other write, so startup is the
only point at which these rows can be written. The write retries with a fresh connection a few
times to ride out transient timing during startup.

For each scheme it writes one Outfits preset and one Attachments preset for the Human species,
each with a row per swatch property colored from the scheme's ColorMap atlas, the same rows
Synty's own packs ship. Idempotent: a re-run replaces a scheme rather than duplicating it.

  python register_color_schemes.py <manifest> <database>

The manifest is one "scheme name<TAB>colormap.png" line per scheme.
"""

import os
import shutil
import sqlite3
import struct
import sys
import time
import traceback
import zlib

OUTFIT_COLOR_GROUPS = (2, 3)   # the toolkit's EColorGroup Outfits and Attachments
LOG_PATH = None


def log(message):
    line = time.strftime("%H:%M:%S ") + message
    if LOG_PATH:
        try:
            with open(LOG_PATH, "a") as handle:
                handle.write(line + "\n")
        except OSError:
            pass
    print(line)


def decode_png(data):
    """Decode an 8-bit PNG to a flat list of (r, g, b) pixels, top-left first. zlib only, no
    image library, since the bundled interpreter ships bare. Handles the truecolor, grayscale,
    and palette types Synty's 32x32 ColorMaps use."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return 0, 0, []
    pos = 8
    width = height = color_type = 0
    idat = bytearray()
    palette = None
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        chunk_type = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if chunk_type == b"IHDR":
            width, height, _bit_depth, color_type = struct.unpack(">IIBB", chunk[:10])
        elif chunk_type == b"PLTE":
            palette = chunk
        elif chunk_type == b"IDAT":
            idat += chunk
        elif chunk_type == b"IEND":
            break

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    stride = width * channels
    raw = zlib.decompress(bytes(idat))

    def paeth(a, b, c):
        p = a + b - c
        pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
        return a if pa <= pb and pa <= pc else (b if pb <= pc else c)

    pixels = []
    previous = bytearray(stride)
    offset = 0
    for _ in range(height):
        filter_type = raw[offset]
        offset += 1
        line = bytearray(raw[offset:offset + stride])
        offset += stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            up_left = previous[i - channels] if i >= channels else 0
            if filter_type == 1:
                line[i] = (line[i] + left) & 255
            elif filter_type == 2:
                line[i] = (line[i] + up) & 255
            elif filter_type == 3:
                line[i] = (line[i] + ((left + up) >> 1)) & 255
            elif filter_type == 4:
                line[i] = (line[i] + paeth(left, up, up_left)) & 255
        previous = line
        for x in range(width):
            sample = line[x * channels:(x + 1) * channels]
            if color_type == 3:
                base = sample[0] * 3
                pixels.append((palette[base], palette[base + 1], palette[base + 2]))
            elif color_type == 0:
                pixels.append((sample[0], sample[0], sample[0]))
            else:
                pixels.append((sample[0], sample[1], sample[2]))
    return width, height, pixels


def swatch_hex(pixels, u, v):
    """Color of swatch cell (u, v) in a 32x32 ColorMap. Each cell is 2x2 pixels, V flipped,
    matching the toolkit's own UV-to-cell lookup."""
    r, g, b = pixels[(31 - v * 2) * 32 + u * 2]
    return "%02X%02X%02X" % (r, g, b)


def read_manifest(path):
    schemes = []
    with open(path, "r") as handle:
        for line in handle:
            name, sep, png = line.rstrip("\n").partition("\t")
            if sep and os.path.isfile(png):
                schemes.append((name, png))
    return schemes


def write_schemes(connection, schemes):
    species = connection.execute("SELECT id FROM sk_species WHERE code='HN'").fetchone()
    if not species:
        raise RuntimeError("Human species not found in database")
    species_id = species[0]
    properties = {
        group: connection.execute(
            "SELECT id, u, v FROM sk_color_property WHERE color_group=?", (group,)).fetchall()
        for group in OUTFIT_COLOR_GROUPS
    }
    for name, pixels in schemes:
        for group in OUTFIT_COLOR_GROUPS:
            for (existing_id,) in connection.execute(
                    "SELECT id FROM sk_color_preset WHERE name=? AND ptr_species=? AND color_group=?",
                    (name, species_id, group)).fetchall():
                connection.execute("DELETE FROM sk_color_preset_row WHERE ptr_color_preset=?", (existing_id,))
                connection.execute("DELETE FROM sk_color_preset WHERE id=?", (existing_id,))
            preset_id = connection.execute(
                "INSERT INTO sk_color_preset (ptr_species, color_group, name) VALUES (?, ?, ?)",
                (species_id, group, name)).lastrowid
            connection.executemany(
                "INSERT INTO sk_color_preset_row "
                "(ptr_color_preset, ptr_color_property, color, metallic, smoothness, reflection, emission, opacity) "
                "VALUES (?, ?, ?, 'FF0000', 'FF0000', 'FF0000', 'FF0000', 'FF0000')",
                [(preset_id, prop_id, swatch_hex(pixels, u, v)) for prop_id, u, v in properties[group]])


def register(db_path, schemes, attempts=90, delay_seconds=2):
    """Retry the whole write with a fresh connection each time. This runs at startup, before the
    toolkit opens and locks the database for the session; the retries only ride out transient
    timing during startup, discarding a connection that opens read-only and trying again."""
    shutil.copy2(db_path, db_path + ".bak")
    last_error = None
    for attempt in range(1, attempts + 1):
        connection = None
        try:
            connection = sqlite3.connect(db_path, timeout=10)
            connection.execute("PRAGMA busy_timeout=10000")
            write_schemes(connection, schemes)
            connection.commit()
            log("registered %d color scheme(s) on attempt %d" % (len(schemes), attempt))
            return True
        except sqlite3.OperationalError as error:
            last_error = error
            if connection:
                try:
                    connection.rollback()
                except sqlite3.Error:
                    pass
            time.sleep(delay_seconds)
        finally:
            if connection:
                try:
                    connection.close()
                except sqlite3.Error:
                    pass
    log("FAILED after %d attempts; database stayed busy. last error: %s" % (attempts, last_error))
    return False


def main():
    global LOG_PATH
    if len(sys.argv) < 3:
        return
    manifest_path, db_path = sys.argv[1], sys.argv[2]
    LOG_PATH = os.path.join(os.path.dirname(manifest_path), "db_register.log")
    try:
        schemes = read_manifest(manifest_path)
        if not schemes:
            log("no schemes in manifest; nothing to do")
            return
        # Insert in name order so the toolkit, which lists presets by insertion order, shows
        # them sorted (01, 02, 03, ...) like a native pack.
        schemes.sort(key=lambda item: item[0])
        if not os.path.isfile(db_path):
            log("database not found: %s" % db_path)
            return
        decoded = []
        for name, png_path in schemes:
            with open(png_path, "rb") as handle:
                _, _, pixels = decode_png(handle.read())
            decoded.append((name, pixels))
        register(db_path, decoded)
    except Exception as error:
        log("ERROR: " + repr(error))
        log(traceback.format_exc())
        raise


if __name__ == "__main__":
    main()
