"""Register a converted pack's color schemes into the Sidekick toolkit database.

Runs under Unreal's bundled standalone Python. The write needs the database unlocked, and the
toolkit holds the connection while its window is open, so the plugin runs this either right
after a conversion with the toolkit closed, or at editor startup before the toolkit opens the
file. It retries with a fresh connection to ride out timing.

From each scheme's ColorMap atlas it writes the Species (skin / robot plating) swatches under the
pack's own species and the Outfit/Attachment swatches under the Human species (the shared pool) -
whichever of those three groups the atlas actually fills. Materials and Elements are never written
from an atlas; they are a base-curated set, and pack colors landing there would mis-file e.g. robot
plating as an Element. To match the Unity tool, where the shared pools show for every species, each
non-Human species the batch converts is then given Human's Outfit/Attachment/Material pools - keyed
off the species being present, not off what its atlas fills, so a species that only ships skin
colors (a zombie wearing human outfits) still gets the full menu. Elements stays Human-only.
Idempotent: a re-run replaces a scheme rather than duplicating it.

  python register_color_schemes.py <manifest> <database> [attempts] [delay_seconds]

The manifest is one "scheme name<TAB>colormap.png<TAB>species_code" line per scheme. The species
code is the sk_species.code of the pack (HN, SN, GO, EV, ZB, RO); a missing third column means HN.
"""

import os
import shutil
import sqlite3
import struct
import sys
import time
import traceback
import zlib

# EColorGroup: 1 Species, 2 Outfits, 3 Attachments, 4 Materials, 5 Elements. Species is per-species
# (human skin / bone properties); the rest are shared/global pools the Unity tool shows for everyone.
SPECIES_COLOR_GROUP = 1               # written under the pack's own species (skin / robot plating)
# What we write from a pack's atlases: Species (under the pack's species) + Outfits/Attachments
# (under Human, the shared pool). Materials and Elements are NOT written from atlases - they are a
# base-curated set ("Materials 01-10", "Elements 01") and writing pack colors into them mis-files
# e.g. robot plating as an Element. They reach other species only through replication below.
REGISTER_GROUPS = (1, 2, 3)
# Shared with every non-Human species so its color menu matches the Unity tool: Outfits, Attachments,
# Materials. Elements is left to Human only (no non-Human official pack shows it), avoiding a stray
# inert category.
REPLICATE_GROUPS = (2, 3, 4)
GLOBAL_COLOR_GROUPS = (2, 3, 4, 5)    # groups cleared of stale replicas before re-replicating
HUMAN_SPECIES_CODE = "HN"
SENTINEL = "FF0000"                   # an unused swatch cell; toolkit treats it as "no color"
FILL_FRACTION = 0.5                   # a scheme registers in a group when >= half its swatches are real
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
            fields = line.rstrip("\n").split("\t")
            if len(fields) < 2 or not os.path.isfile(fields[1]):
                continue
            name, png = fields[0], fields[1]
            species_code = fields[2] if len(fields) > 2 and fields[2] else HUMAN_SPECIES_CODE
            schemes.append((name, png, species_code))
    return schemes


def species_id_for(connection, code, cache):
    """sk_species.id for a species code, cached. None if the code is unknown."""
    if code not in cache:
        row = connection.execute("SELECT id FROM sk_species WHERE code=?", (code,)).fetchone()
        cache[code] = row[0] if row else None
    return cache[code]


def replace_preset(connection, species_id, group, name, prop_colors):
    """Write one color preset (replacing any same name/species/group), one row per property."""
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
        [(preset_id, prop_id, color) for prop_id, color in prop_colors])


def replicate_global_pools(connection, human_id, species_id, fill_groups):
    """Give a non-Human species the same shared Outfit/Attachment/Material/Element pools as Human,
    but only for the groups the pack's own atlases actually use (fill_groups), so the species shows
    exactly the color categories it needs — like the official tool, where a Skeleton gets Outfits/
    Attachments/Materials but not Elements. Replicas that share a Human name are refreshed; any stale
    replica in a group the pack no longer fills is removed; the species' own unique presets (e.g.
    Goblin's) are left untouched. Idempotent."""
    for group in GLOBAL_COLOR_GROUPS:
        human_presets = connection.execute(
            "SELECT id, name FROM sk_color_preset WHERE ptr_species=? AND color_group=?",
            (human_id, group)).fetchall()
        # Drop any prior replica of a Human-named preset first, so a group that is no longer in
        # fill_groups (e.g. Elements) is cleared out rather than left stale.
        for _src_id, name in human_presets:
            for (existing_id,) in connection.execute(
                    "SELECT id FROM sk_color_preset WHERE name=? AND ptr_species=? AND color_group=?",
                    (name, species_id, group)).fetchall():
                connection.execute("DELETE FROM sk_color_preset_row WHERE ptr_color_preset=?", (existing_id,))
                connection.execute("DELETE FROM sk_color_preset WHERE id=?", (existing_id,))
        if group not in fill_groups:
            continue
        for src_id, name in human_presets:
            new_id = connection.execute(
                "INSERT INTO sk_color_preset (ptr_species, color_group, name) VALUES (?, ?, ?)",
                (species_id, group, name)).lastrowid
            connection.execute(
                "INSERT INTO sk_color_preset_row "
                "(ptr_color_preset, ptr_color_property, color, metallic, smoothness, reflection, emission, opacity) "
                "SELECT ?, ptr_color_property, color, metallic, smoothness, reflection, emission, opacity "
                "FROM sk_color_preset_row WHERE ptr_color_preset=?",
                (new_id, src_id))


def write_schemes(connection, schemes):
    human_id = species_id_for(connection, HUMAN_SPECIES_CODE, {})
    if not human_id:
        raise RuntimeError("Human species not found in database")
    species_cache = {HUMAN_SPECIES_CODE: human_id}
    properties = {
        group: connection.execute(
            "SELECT id, u, v FROM sk_color_property WHERE color_group=?", (group,)).fetchall()
        for group in REGISTER_GROUPS
    }
    non_human_species = set()
    for name, pixels, species_code in schemes:
        own_id = species_id_for(connection, species_code, species_cache) or human_id
        if own_id != human_id:
            non_human_species.add(own_id)
        for group in REGISTER_GROUPS:
            props = properties[group]
            if not props:
                continue
            colors = [(prop_id, swatch_hex(pixels, u, v)) for prop_id, u, v in props]
            real = sum(1 for _, hex_color in colors if hex_color != SENTINEL)
            if real < FILL_FRACTION * len(props):
                continue  # this atlas doesn't fill this group (e.g. a skeleton has no skin colors)
            # Species (skin / plating) colors go under the pack's own species; the shared outfit
            # pools go under Human. Materials/Elements are base-curated and never written here.
            dest_id = own_id if group == SPECIES_COLOR_GROUP else human_id
            replace_preset(connection, dest_id, group, name, colors)
    # Give every non-Human species the shared Outfit/Attachment/Material pools, like the Unity tool
    # shows for all species. Keyed off the species being present (not what its atlas fills), so a
    # species that only ships skin colors (zombies wear human outfits) still gets the full menu.
    for species_id in non_human_species:
        replicate_global_pools(connection, human_id, species_id, REPLICATE_GROUPS)


def register(db_path, schemes, attempts=90, delay_seconds=2):
    """Retry the whole write with a fresh connection each time, to ride out the database being
    locked by the toolkit. A connection that opens read-only is discarded and retried."""
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
        return 1
    manifest_path, db_path = sys.argv[1], sys.argv[2]
    attempts = int(sys.argv[3]) if len(sys.argv) > 3 else 90
    delay_seconds = float(sys.argv[4]) if len(sys.argv) > 4 else 2
    LOG_PATH = os.path.join(os.path.dirname(manifest_path), "db_register.log")
    try:
        schemes = read_manifest(manifest_path)
        if not schemes:
            log("no schemes in manifest; nothing to do")
            return 0
        # Insert in name order so the toolkit, which lists presets by insertion order, shows
        # them sorted (01, 02, 03, ...) like a native pack.
        schemes.sort(key=lambda item: item[0])
        if not os.path.isfile(db_path):
            # The toolkit creates this database the first time it runs, so on a project that
            # has never opened it the file is not there yet. A nonzero exit keeps the caller
            # from deleting the manifest, so a later startup applies it once the file exists.
            log("database not found, leaving schemes for a later startup: %s" % db_path)
            return 1
        decoded = []
        for name, png_path, species_code in schemes:
            with open(png_path, "rb") as handle:
                _, _, pixels = decode_png(handle.read())
            decoded.append((name, pixels, species_code))
        return 0 if register(db_path, decoded, attempts, delay_seconds) else 1
    except Exception as error:
        log("ERROR: " + repr(error))
        log(traceback.format_exc())
        return 1


if __name__ == "__main__":
    sys.exit(main())
