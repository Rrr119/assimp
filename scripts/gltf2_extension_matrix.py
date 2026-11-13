#!/usr/bin/env python3
"""Scan glTF 2.0 test assets and print the extension support matrix."""

from __future__ import annotations

import argparse
import json
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class ExtensionSupport:
    import_status: str
    export_status: str
    notes: str


EXTENSION_SUPPORT = {
    "KHR_lights_punctual": ExtensionSupport(
        "Full",
        "None",
        "Punctual lights map to aiLight; range is kept as PBR_LightRange node metadata. Lights are not exported.",
    ),
    "KHR_materials_pbrSpecularGlossiness": ExtensionSupport(
        "Full",
        "Opt-in",
        "Deprecated. Export requires AI_CONFIG_USE_GLTF_PBR_SPECULAR_GLOSSINESS; the default export uses KHR_materials_specular.",
    ),
    "KHR_materials_specular": ExtensionSupport(
        "Full",
        "Full",
        "Not emitted for unlit materials or when pbrSpecularGlossiness is active.",
    ),
    "KHR_materials_unlit": ExtensionSupport(
        "Full",
        "Full",
        "Mutually exclusive with other material extensions during export.",
    ),
    "KHR_texture_transform": ExtensionSupport(
        "Partial",
        "None",
        "Imported as aiUVTransform with glTF-to-Assimp coordinate conversion; the extension texCoord override and export are not supported.",
    ),
    "KHR_materials_sheen": ExtensionSupport(
        "Full",
        "Full",
        "A zero sheen color factor is treated as disabled.",
    ),
    "KHR_materials_clearcoat": ExtensionSupport(
        "Full",
        "Full",
        "A zero clearcoat factor is treated as disabled.",
    ),
    "KHR_materials_transmission": ExtensionSupport(
        "Full",
        "Full",
        "Maps factor and texture through AI_MATKEY_TRANSMISSION_*.",
    ),
    "KHR_materials_volume": ExtensionSupport(
        "Full",
        "Full",
        "Maps thickness, attenuation distance and attenuation color.",
    ),
    "KHR_materials_ior": ExtensionSupport(
        "Full",
        "Full",
        "Maps IOR to AI_MATKEY_REFRACTI.",
    ),
    "KHR_materials_emissive_strength": ExtensionSupport(
        "Full",
        "Full",
        "Maps emissive strength to AI_MATKEY_EMISSIVE_INTENSITY.",
    ),
    "KHR_materials_anisotropy": ExtensionSupport(
        "Full",
        "Full",
        "Zero anisotropy strength is treated as disabled.",
    ),
    "KHR_draco_mesh_compression": ExtensionSupport(
        "Gated",
        "None",
        "Requires ASSIMP_BUILD_DRACO=ON. Decoding is limited to TRIANGLES and TRIANGLE_STRIP primitives.",
    ),
    "KHR_texture_basisu": ExtensionSupport(
        "Partial",
        "Partial",
        "KTX2 payloads are passed through as raw aiTexture data, not transcoded. Export marks the extension as required.",
    ),
    "EXT_texture_webp": ExtensionSupport(
        "Partial",
        "None",
        "WebP payloads are passed through as raw texture data; pixels are not decoded and the extension is not exported.",
    ),
    "FB_ngon_encoding": ExtensionSupport(
        "None",
        "Partial",
        "Export-only marker for aiPrimitiveType_NGONEncodingFlag primitives; it is listed in extensionsUsed for every export.",
    ),
}


def read_gltf_json(path: Path) -> tuple[dict, bool]:
    data = path.read_bytes()
    if data[:4] != b"glTF":
        if path.suffix.lower() == ".gltf":
            return json.loads(data.decode("utf-8-sig")), False
        raise ValueError("not a glTF or GLB asset")

    if len(data) < 12:
        raise ValueError("invalid GLB header")

    version, declared_length = struct.unpack_from("<II", data, 4)
    if version != 2:
        raise ValueError(f"unsupported GLB version {version}")
    if declared_length > len(data):
        raise ValueError("truncated GLB")

    offset = 12
    while offset + 8 <= declared_length:
        chunk_length, chunk_type = struct.unpack_from("<II", data, offset)
        chunk_start = offset + 8
        chunk_end = chunk_start + chunk_length
        if chunk_end > declared_length:
            raise ValueError("truncated GLB chunk")
        if chunk_type == 0x4E4F534A:
            payload = data[chunk_start:chunk_end].rstrip(b" \t\r\n\x00")
            return json.loads(payload.decode("utf-8-sig")), True
        offset = chunk_end

    raise ValueError("GLB has no JSON chunk")


def is_generated_asset(path: Path) -> bool:
    stem = path.stem.lower()
    return stem.endswith("_out") or stem.endswith(".gltf_out")


def model_files(models_dir: Path, include_generated: bool) -> Iterable[Path]:
    return sorted(
        path
        for path in models_dir.rglob("*")
        if path.is_file() and path.suffix.lower() in {".gltf", ".glb"}
        and (include_generated or not is_generated_asset(path))
    )


def string_array(document: dict, member: str) -> list[str]:
    value = document.get(member, [])
    if not isinstance(value, list):
        return []
    return [entry for entry in value if isinstance(entry, str)]


def scan_models(
    models_dir: Path, include_generated: bool
) -> tuple[dict[str, dict[str, list[str]]], int, int]:
    usage: dict[str, dict[str, list[str]]] = {}
    gltf_count = 0
    glb_count = 0

    for path in model_files(models_dir, include_generated):
        try:
            document, is_binary = read_gltf_json(path)
        except (OSError, UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
            print(f"warning: skipping {path}: {error}", file=sys.stderr)
            continue

        if is_binary:
            glb_count += 1
        else:
            gltf_count += 1

        relative_path = str(path.relative_to(models_dir))
        for member, usage_key in (
            ("extensionsUsed", "used"),
            ("extensionsRequired", "required"),
        ):
            for extension in string_array(document, member):
                entry = usage.setdefault(
                    extension, {"used": [], "required": []}
                )
                entry[usage_key].append(relative_path)

    return usage, gltf_count, glb_count


def support_for(extension: str) -> ExtensionSupport:
    return EXTENSION_SUPPORT.get(
        extension,
        ExtensionSupport(
            "None",
            "None",
            "No parser support; import fails when listed in extensionsRequired.",
        ),
    )


def render_markdown(
    usage: dict[str, dict[str, list[str]]], gltf_count: int, glb_count: int
) -> str:
    extensions = sorted(set(EXTENSION_SUPPORT) | set(usage))
    lines = [
        "| Extension | Import | Export | Models used | Models required | Notes |",
        "| --- | --- | --- | ---: | ---: | --- |",
    ]
    for extension in extensions:
        support = support_for(extension)
        entry = usage.get(extension, {"used": [], "required": []})
        lines.append(
            f"| `{extension}` | {support.import_status} | {support.export_status} | "
            f"{len(entry['used'])} | {len(entry['required'])} | {support.notes} |"
        )

    lines.extend(
        [
            "",
            f"Scanned {gltf_count + glb_count} assets "
            f"({gltf_count} .gltf, {glb_count} .glb); "
            f"observed {len(usage)} extension IDs.",
        ]
    )
    return "\n".join(lines)


def render_json(
    usage: dict[str, dict[str, list[str]]], gltf_count: int, glb_count: int
) -> str:
    extensions = []
    for extension in sorted(set(EXTENSION_SUPPORT) | set(usage)):
        support = support_for(extension)
        entry = usage.get(extension, {"used": [], "required": []})
        extensions.append(
            {
                "name": extension,
                "import": support.import_status,
                "export": support.export_status,
                "notes": support.notes,
                "modelsUsed": entry["used"],
                "modelsRequired": entry["required"],
            }
        )
    return json.dumps(
        {
            "assetsScanned": gltf_count + glb_count,
            "gltfAssets": gltf_count,
            "glbAssets": glb_count,
            "extensions": extensions,
        },
        indent=2,
    )


def main() -> int:
    default_models_dir = Path(__file__).resolve().parents[1] / "test" / "models" / "glTF2"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--models-dir",
        type=Path,
        default=default_models_dir,
        help=f"glTF 2.0 test model directory (default: {default_models_dir})",
    )
    parser.add_argument("--format", choices=("markdown", "json"), default="markdown")
    parser.add_argument("--output", type=Path, help="write the matrix to a file")
    parser.add_argument(
        "--include-generated",
        action="store_true",
        help="include generated *_out.gltf/*_out.glb test artifacts",
    )
    args = parser.parse_args()

    if not args.models_dir.is_dir():
        parser.error(f"model directory does not exist: {args.models_dir}")

    usage, gltf_count, glb_count = scan_models(
        args.models_dir, args.include_generated
    )
    matrix = (
        render_markdown(usage, gltf_count, glb_count)
        if args.format == "markdown"
        else render_json(usage, gltf_count, glb_count)
    )

    if args.output:
        args.output.write_text(matrix + "\n", encoding="utf-8")
    else:
        print(matrix)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
