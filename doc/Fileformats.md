#### Supported file formats ####

__Importers__:

- 3D
- [3DS](https://en.wikipedia.org/wiki/.3ds)
- [3MF](https://en.wikipedia.org/wiki/3D_Manufacturing_Format)
- AC
- [AC3D](https://en.wikipedia.org/wiki/AC3D)
- ACC
- AMJ
- ASE
- ASK
- B3D
- [BVH](https://en.wikipedia.org/wiki/Biovision_Hierarchy)
- CSM
- COB
- [DAE/Collada](https://en.wikipedia.org/wiki/COLLADA)
- [DXF](https://en.wikipedia.org/wiki/AutoCAD_DXF)
- ENFF
- [FBX](https://en.wikipedia.org/wiki/FBX)
- [glTF 1.0](https://en.wikipedia.org/wiki/GlTF#glTF_1.0) + GLB
- [glTF 2.0](https://en.wikipedia.org/wiki/GlTF#glTF_2.0) + GLB (see [glTF 2.0 extension support](#gltf-20-extension-support))
- HMB
- IFC-STEP
- IQM
- IRR / IRRMESH
- [LWO](https://en.wikipedia.org/wiki/LightWave_3D)
- LWS
- LXO
- [M3D](https://bztsrc.gitlab.io/model3d)
   - **REQUIRES OPT-IN:** enable cmake options `ASSIMP_BUILD_M3D_IMPORTER`/`ASSIMP_BUILD_M3D_EXPORTER`
      - NOTE: author of `M3D` format abandoned assimp support; implementation is incomplete and buggy.
- MD2
- MD3
- MD5
- MDC
- MDL
- MESH / MESH.XML
- MOT
- MS3D
- NDO
- NFF
- [OBJ](https://en.wikipedia.org/wiki/Wavefront_.obj_file)
- [OFF](https://en.wikipedia.org/wiki/OFF_(file_format))
- [OGEX](https://en.wikipedia.org/wiki/Open_Game_Engine_Exchange)
- [PLY](https://en.wikipedia.org/wiki/PLY_(file_format))
- PMX
- PRJ
- Q3O
- Q3S
- RAW
- SCN
- SIB
- SMD
- [STP](https://en.wikipedia.org/wiki/ISO_10303-21)
- [STL](https://en.wikipedia.org/wiki/STL_(file_format))
- TER
- UC
- [USD](https://en.wikipedia.org/wiki/Universal_Scene_Description)
   - **REQUIRES OPT-IN:** enable cmake option `ASSIMP_BUILD_USD_IMPORTER`
   - To use latest v1.0.0 USD release also enable cmake option `ASSIMP_USD_BETA`
- VRML/WRL
   - **REQUIRES OPT-IN:** enable cmake option `ASSIMP_BUILD_VRML_IMPORTER`
- VTA
- X
- [X3D](https://en.wikipedia.org/wiki/X3D)
- XGL
- ZGL

Note: support for [BLEND](https://en.wikipedia.org/wiki/.blend_(file_format)) is deprecated.
It is too time-consuming to maintain an undocumented format which contains so much more than we need.

Additionally, some formats are supported by dependency on non-free code or external SDKs (not built by default):

- [C4D](https://en.wikipedia.org/wiki/Cinema_4D) (https://github.com/assimp/assimp/wiki/Cinema4D-&-Melange) IMporting geometry + node hierarchy are currently supported

__Exporters__:

- DAE (Collada)
- STL
- OBJ
- PLY
- X
- 3DS
- JSON (for WebGl, via https://github.com/acgessler/assimp2json)
- ASSBIN
- STEP
- [PBRTv4](https://github.com/mmp/pbrt-v4)
- glTF 1.0 (partial)
- glTF 2.0 (partial)
- 3MF ( experimental )
- FBX ( experimental )

#### glTF 2.0 extension support ####

The table covers the extensions wired into the glTF 2.0 importer and exporter. Status values:

- **Full**: the extension data is mapped to Assimp scene/material data, or emitted from it.
- **Partial**: only part of the extension is handled. Raw/compressed texture payloads are not decoded.
- **Gated**: support depends on a build option.
- **Opt-in**: export only happens when the documented export property is enabled.
- **None**: the extension is not handled by that direction.

| Extension | Import | Export | Limitations |
| --- | --- | --- | --- |
| `KHR_lights_punctual` | Full | None | Punctual lights map to `aiLight`; `range` is stored as `PBR_LightRange` node metadata. Lights are not exported. |
| `KHR_materials_pbrSpecularGlossiness` | Full | Opt-in | Deprecated. Export requires `AI_CONFIG_USE_GLTF_PBR_SPECULAR_GLOSSINESS`; the default export uses `KHR_materials_specular`. |
| `KHR_materials_specular` | Full | Full | Not emitted for unlit materials or when `KHR_materials_pbrSpecularGlossiness` is active. |
| `KHR_materials_unlit` | Full | Full | Mutually exclusive with other material extensions during export. |
| `KHR_texture_transform` | Partial | None | Imported as `aiUVTransform` with glTF-to-Assimp coordinate conversion. The extension's `texCoord` override and export are not supported. |
| `KHR_materials_sheen` | Full | Full | A zero sheen color factor is treated as disabled on export. |
| `KHR_materials_clearcoat` | Full | Full | A zero clearcoat factor is treated as disabled on export. |
| `KHR_materials_transmission` | Full | Full | Maps factor and texture through `AI_MATKEY_TRANSMISSION_*`. |
| `KHR_materials_volume` | Full | Full | Maps thickness, attenuation distance and attenuation color to the corresponding Assimp material keys. |
| `KHR_materials_ior` | Full | Full | Maps IOR to `AI_MATKEY_REFRACTI`. |
| `KHR_materials_emissive_strength` | Full | Full | Maps emissive strength to `AI_MATKEY_EMISSIVE_INTENSITY`. |
| `KHR_materials_anisotropy` | Full | Full | A zero anisotropy strength is treated as disabled on export. |
| `KHR_draco_mesh_compression` | Gated | None | Requires `ASSIMP_BUILD_DRACO=ON`. Decoding is limited to `TRIANGLES` and `TRIANGLE_STRIP` primitives. |
| `KHR_texture_basisu` | Partial | Partial | KTX2 payloads are passed through as raw `aiTexture` data, not transcoded. Export marks the extension as required. |
| `EXT_texture_webp` | Partial | None | WebP payloads are passed through as raw texture data; pixels are not decoded and the extension is not exported. |
| `FB_ngon_encoding` | None | Partial | Export-only marker for `aiPrimitiveType_NGONEncodingFlag` primitives; it is listed in `extensionsUsed` for every export. |

Unknown extension IDs in `extensionsUsed` are ignored, as permitted by the glTF specification. An unknown ID in
`extensionsRequired` makes the import fail with `GLTF: Unsupported required extension: <id>`; this includes
`KHR_technique_webgl`, which is kept as a negative test fixture.

The extension matrix can be regenerated from the glTF 2.0 test assets. It scans both `.gltf` JSON files and the JSON
chunk of `.glb` files:

```bash
python3 scripts/gltf2_extension_matrix.py
python3 scripts/gltf2_extension_matrix.py --format json
```
