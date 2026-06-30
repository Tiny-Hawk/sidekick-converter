"""Convert a Synty Sidekick Unity pack into Unreal parts on the shared skeleton.

One pass, no Blender or FBX round-trip:
  1. Extract the part meshes from the .unitypackage.
  2. Import each part (mesh and morph targets, no materials).
  3. Conform it to SKEL_Default_Sidekick in-engine, rebuilding the part's bones onto the
     shared skeleton's canonical rest pose and keeping the bind pose, skin weights, and
     jiggle bones.
  4. Import the pack's ColorMap atlases and build a material instance per color scheme, then
     leave the scheme list in a manifest.

The panel writes those schemes into the toolkit database when the conversion finishes. If the
toolkit is holding the database open, the write falls back to the next editor startup.

The shared skeleton, orientation reference part, and material come from an installed
Sidekick free pack.

Run headless with the editor closed:
  UnrealEditor-Cmd <uproject> -ExecutePythonScript=".../convert_sidekick_pack.py" -unattended -nopause -nosplash -nullrhi
"""

import unreal
import os
import re
import tarfile

# The editor panel writes the job here (which pack to convert and which shared assets to
# conform onto) and this script reports progress back for the panel to poll.
JOB_DIR = os.path.join(unreal.Paths.project_saved_dir(), "SidekickConverter")
JOB_FILE = os.path.join(JOB_DIR, "job.txt")
PROGRESS_FILE = os.path.join(JOB_DIR, "progress.txt")
SCHEME_MANIFEST = os.path.join(JOB_DIR, "schemes.txt")

# Raw files pulled from the pack are extracted here, not into Content, so the engine's
# source-asset watcher never offers to reimport them over the converted assets.
PARTS_TEMP = os.path.join(JOB_DIR, "parts")
COLORMAP_TEMP = os.path.join(JOB_DIR, "colormaps")

# Defaults for a standard Sidekick free-pack install; the panel can override them.
DEFAULTS = {
    "PACKAGE": "",
    "SKELETON": "/Game/Synty/SidekickCharacters/Resources/Skeletons/SKEL_Default_Sidekick",
    "REFERENCE": "/Game/Synty/SidekickCharacters/Resources/Meshes/Species/Humans/SK_HUMN_BASE_01_10TORS_HU01",
    "MATERIAL": "/Game/Synty/SidekickCharacters/Resources/Materials/M_Default_Sidekick",
}

# The color atlas parameter on M_Default_Sidekick, set per scheme material instance.
COLOR_TEXTURE_PARAM = "texture"


def read_job():
    """job.txt is KEY=VALUE lines. PACKAGE may repeat (a batch of packs); SKELETON/
    REFERENCE/MATERIAL are single. Missing keys default."""
    job = dict(DEFAULTS)
    packages = []
    if os.path.isfile(JOB_FILE):
        with open(JOB_FILE, "r") as handle:
            for line in handle:
                key, sep, value = line.strip().partition("=")
                if not sep or not value:
                    continue
                if key == "PACKAGE":
                    packages.append(value)
                elif key in job:
                    job[key] = value
    job["PACKAGES"] = packages
    return job


def report_progress(current, total, name):
    """One line the panel polls: current<TAB>total<TAB>name. 'name' is DONE when finished."""
    try:
        if not os.path.isdir(JOB_DIR):
            os.makedirs(JOB_DIR)
        with open(PROGRESS_FILE, "w") as progress:
            progress.write("%d\t%d\t%s" % (current, total, name))
    except OSError:
        pass


JOB = read_job()
PACKAGES = JOB["PACKAGES"]
SHARED_SKELETON = JOB["SKELETON"]
ORIENTATION_REFERENCE = JOB["REFERENCE"]
SHARED_MATERIAL = JOB["MATERIAL"]


def log(message):
    unreal.log("[SIDEKICK] " + message)


def extract_part_meshes(unitypackage_path):
    """Pull every part FBX out of the .unitypackage into a scratch folder, preserving Synty's
    Assets/Synty/... layout. Returns (filesystem path, /Game destination path, asset name)."""
    archive = tarfile.open(unitypackage_path, "r:gz")

    guid_to_path = {}
    for member in archive.getmembers():
        if member.name.endswith("/pathname"):
            guid = member.name.split("/")[0]
            guid_to_path[guid] = archive.extractfile(member).read().decode("utf-8").strip()

    parts = []
    for member in archive.getmembers():
        if not member.name.endswith("/asset"):
            continue
        unity_path = guid_to_path.get(member.name.split("/")[0], "")
        # The pack's own part meshes: outfit pieces, plus a non-human species' base body
        # (head/skull, eyes, teeth, base limbs) when the pack ships one. The shared Humans
        # base body is pre-installed with the free pack and is the conform's orientation
        # reference, so it stays skipped; every other Species/<X> base is the pack's own
        # content and must come in, or that species has no head and shows None in the toolkit.
        lower_path = unity_path.lower()
        in_outfits = "/resources/meshes/outfits/" in lower_path
        in_species = "/resources/meshes/species/" in lower_path and "/resources/meshes/species/humans/" not in lower_path
        if not lower_path.endswith(".fbx") or not (in_outfits or in_species):
            continue

        relative = unity_path[len("Assets/"):]              # Synty/.../Outfits/X/SK_....fbx
        disk_path = os.path.join(PARTS_TEMP, relative)
        os.makedirs(os.path.dirname(disk_path), exist_ok=True)
        with open(disk_path, "wb") as out_file:
            out_file.write(archive.extractfile(member).read())

        game_dir = "/Game/" + os.path.dirname(relative).replace("\\", "/")
        name = os.path.splitext(os.path.basename(relative))[0]
        parts.append((disk_path, game_dir, name))

    archive.close()
    return parts


def mesh_import_options():
    """Mesh + morph targets only. Morphs carry Synty's body-shape blends; materials and
    textures are skipped because the FBX only references throwaway Unity materials and
    the real material is the shared M_Default_Sidekick assigned afterwards."""
    options = unreal.FbxImportUI()
    options.set_editor_property("import_mesh", True)
    options.set_editor_property("import_as_skeletal", True)
    options.set_editor_property("import_materials", False)
    options.set_editor_property("import_textures", False)
    options.set_editor_property("import_animations", False)
    options.set_editor_property("create_physics_asset", False)
    skeletal_data = options.get_editor_property("skeletal_mesh_import_data")
    skeletal_data.set_editor_property("import_morph_targets", True)
    options.set_editor_property("skeletal_mesh_import_data", skeletal_data)
    return options


def import_part(fbx_path, game_dir, name):
    task = unreal.AssetImportTask()
    task.filename = fbx_path
    task.destination_path = game_dir
    task.destination_name = name
    task.automated = True
    task.save = False
    task.replace_existing = True
    task.options = mesh_import_options()
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    return next((obj for obj in task.get_objects() if isinstance(obj, unreal.SkeletalMesh)), None)


def assign_material(mesh, material):
    new_materials = []
    for index in range(len(mesh.get_editor_property("materials"))):
        slot = unreal.SkeletalMaterial()
        slot.set_editor_property("material_interface", material)
        slot.set_editor_property("material_slot_name", "COLOR" if index == 0 else "COLOR_%d" % index)
        new_materials.append(slot)
    mesh.set_editor_property("materials", new_materials)
    mesh.modify()


def pack_of_part(game_dir):
    """The pack folder under .../Outfits/, e.g. ModernCivilians."""
    return game_dir.rstrip("/").split("/")[-1]


def scheme_name(colormap_asset):
    """A ColorMap asset like T_ModernCivilian_03ColorMap becomes 'Modern Civilian 03',
    matching the spacing of Synty's shipped scheme names (e.g. 'Apocalypse Outlaws 01')."""
    stem = colormap_asset
    if stem.startswith("T_"):
        stem = stem[len("T_"):]
    stem = stem.replace("ColorMap", "")
    base, _, index = stem.rpartition("_")
    if not base:
        base = stem
        index = ""
    spaced = re.sub(r"(?<!^)(?=[A-Z])", " ", base)
    return ("%s %s" % (spaced, index)).strip()


def extract_color_maps(unitypackage_path, pack_names):
    """Pull the pack's ColorMap atlases (under Characters/<pack>/) into a scratch folder.
    Returns (png path, asset name, scheme name, pack) per atlas. Only atlases whose pack
    matches a converted outfit pack are taken, so the species' base skin maps are left alone."""
    archive = tarfile.open(unitypackage_path, "r:gz")

    guid_to_path = {}
    for member in archive.getmembers():
        if member.name.endswith("/pathname"):
            guid_to_path[member.name.split("/")[0]] = archive.extractfile(member).read().decode("utf-8").strip()

    maps = []
    for member in archive.getmembers():
        if not member.name.endswith("/asset"):
            continue
        unity_path = guid_to_path.get(member.name.split("/")[0], "")
        if not unity_path.lower().endswith("colormap.png"):
            continue
        segments = unity_path.split("/")
        if "Characters" not in segments:
            continue
        pack = segments[segments.index("Characters") + 1]
        if pack not in pack_names:
            continue

        asset_name = os.path.splitext(os.path.basename(unity_path))[0]
        png_path = os.path.join(COLORMAP_TEMP, pack, os.path.basename(unity_path))
        os.makedirs(os.path.dirname(png_path), exist_ok=True)
        with open(png_path, "wb") as out_file:
            out_file.write(archive.extractfile(member).read())
        maps.append((png_path, asset_name, scheme_name(asset_name), pack))

    archive.close()
    return maps


def import_color_map_texture(png_path, pack, asset_name):
    game_dir = "/Game/Synty/SidekickCharacters/Resources/Textures/Outfits/" + pack
    task = unreal.AssetImportTask()
    task.filename = png_path
    task.destination_path = game_dir
    task.destination_name = asset_name
    task.automated = True
    task.save = False
    task.replace_existing = True
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    texture = next((obj for obj in task.get_objects() if isinstance(obj, unreal.Texture2D)), None)
    if not texture:
        return None
    # The atlas is a lookup table, not an image: no filtering, no mips, no compression, or
    # the swatch colors bleed and quantize. Matches the toolkit's ColorMap import.
    texture.set_editor_property("srgb", True)
    texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_VECTOR_DISPLACEMENTMAP)
    texture.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    texture.set_editor_property("filter", unreal.TextureFilter.TF_NEAREST)
    unreal.EditorAssetLibrary.save_asset(texture.get_path_name(), only_if_is_dirty=False)
    return texture


def make_scheme_material(asset_name, pack, texture, base_material):
    """A material instance of M_Default_Sidekick with the scheme's atlas bound, so a part
    carrying it shows the pack's colors without the toolkit in the loop. Reuses an existing
    instance on a re-run, since create_asset returns nothing if it already exists."""
    mic_name = "MI_" + asset_name[len("T_"):].replace("ColorMap", "")
    game_dir = "/Game/Synty/SidekickCharacters/Resources/Materials/Outfits/" + pack
    asset_path = game_dir + "/" + mic_name
    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        mic = unreal.load_asset(asset_path)
    else:
        mic = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            mic_name, game_dir, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    if not mic:
        return None
    unreal.MaterialEditingLibrary.set_material_instance_parent(mic, base_material)
    unreal.MaterialEditingLibrary.set_material_instance_texture_parameter_value(mic, COLOR_TEXTURE_PARAM, texture)
    unreal.EditorAssetLibrary.save_asset(mic.get_path_name(), only_if_is_dirty=False)
    return mic


def build_pack_schemes(color_maps, base_material):
    """Import each atlas and build its scheme material instance. Returns a default scheme
    material per pack so parts come in colored."""
    pack_default = {}
    for png_path, asset_name, name, pack in color_maps:
        texture = import_color_map_texture(png_path, pack, asset_name)
        if not texture:
            continue
        material = make_scheme_material(asset_name, pack, texture, base_material)
        if pack not in pack_default or name < pack_default[pack][0]:
            pack_default[pack] = (name, material)
    return {pack: material for pack, (_, material) in pack_default.items()}


def write_scheme_manifest(manifest):
    """Leave the schemes for register_color_schemes.py, which the plugin's module runs at the
    next editor startup to write them into the toolkit database, before the toolkit opens it.
    The manifest is scheme_name<TAB>colormap.png lines."""
    if not manifest:
        return
    if not os.path.isdir(JOB_DIR):
        os.makedirs(JOB_DIR)
    with open(SCHEME_MANIFEST, "w") as handle:
        for name, png_path in manifest:
            handle.write("%s\t%s\n" % (name, png_path))


def run():
    packages = [p for p in PACKAGES if p and os.path.isfile(p)]
    if not packages:
        log("ABORT: no valid .unitypackage to convert: %s" % PACKAGES)
        report_progress(0, 0, "DONE")
        return

    assets = unreal.EditorAssetLibrary
    shared_skeleton = unreal.load_asset(SHARED_SKELETON)
    orientation_reference = unreal.load_asset(ORIENTATION_REFERENCE)
    shared_material = unreal.load_asset(SHARED_MATERIAL)
    if not (shared_skeleton and orientation_reference and shared_material):
        log("ABORT: missing a required shared asset (skeleton, reference, or material). Install the Sidekick free pack.")
        report_progress(0, 0, "DONE")
        return

    # Extract every pack first so the progress total covers the whole batch.
    report_progress(0, 0, "extracting")
    parts = []
    for package in packages:
        extracted = extract_part_meshes(package)
        log("extracted %d part meshes from %s" % (len(extracted), os.path.basename(package)))
        parts += extracted

    pack_names = {pack_of_part(game_dir) for _, game_dir, _ in parts}
    color_maps = []
    for package in packages:
        color_maps += extract_color_maps(package, pack_names)

    manifest = [(name, png_path) for png_path, _asset, name, _pack in color_maps]
    write_scheme_manifest(manifest)

    pack_default_material = build_pack_schemes(color_maps, shared_material)
    log("found %d color scheme(s) across %d pack(s)" % (len(manifest), len(pack_names)))

    total = len(parts)
    report_progress(0, total, "starting")

    converted, failed = 0, []
    for index, (fbx_path, game_dir, name) in enumerate(parts):
        report_progress(index + 1, total, name)
        log("Importing: %s  (%d/%d)" % (name, index + 1, total))
        mesh = import_part(fbx_path, game_dir, name)
        if not mesh:
            failed.append(name)
            continue

        orphan_skeleton = mesh.get_editor_property("skeleton")
        if not unreal.SidekickConverterLibrary.conform_to_shared_skeleton(mesh, shared_skeleton, orientation_reference):
            failed.append(name)
            continue
        assign_material(mesh, pack_default_material.get(pack_of_part(game_dir), shared_material))

        # Build a per-mesh physics asset like the official Unreal packs ship. Done after the
        # conform so the bodies fit the shared skeleton; a prior one is removed first so a
        # re-convert regenerates cleanly.
        physics_path = "%s/%s_PhysicsAsset" % (game_dir, name)
        if assets.does_asset_exist(physics_path):
            assets.delete_asset(physics_path)
        physics_asset = unreal.SidekickConverterLibrary.create_physics_asset_for_mesh(mesh)

        assets.save_asset(mesh.get_path_name(), only_if_is_dirty=False)
        if physics_asset:
            assets.save_asset(physics_asset.get_path_name(), only_if_is_dirty=False)
        else:
            log("WARN: physics asset not created for %s" % name)

        if orphan_skeleton and orphan_skeleton.get_path_name() != shared_skeleton.get_path_name():
            stem = orphan_skeleton.get_path_name().split(".")[0]
            if assets.does_asset_exist(stem):
                assets.delete_asset(stem)

        converted += 1
        if converted % 10 == 0:
            unreal.SystemLibrary.collect_garbage()

    report_progress(total, total, "DONE")
    log("DONE. converted=%d failed=%d" % (converted, len(failed)))
    if failed:
        log("FAILED: %s" % failed)


run()
