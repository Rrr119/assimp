#include "BakeTextureAtlas.h"

#ifndef ASSIMP_BUILD_NO_BAKETEXTUREATLAS_PROCESS

#include "Common/StbCommon.h"

#include <assimp/Exceptional.h>
#include <assimp/DefaultLogger.hpp>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/texture.h>

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Assimp {

namespace {

constexpr std::size_t kDefaultMaxAtlasSize = 2048;
constexpr std::size_t kDefaultPadding = 1;
constexpr char kDefaultAtlasFilename[] = "assimp_basecolor_atlas.png";

struct AtlasRect {
    std::size_t x = 0;
    std::size_t y = 0;
    std::size_t width = 0;
    std::size_t height = 0;
};

struct SourceTexture {
    std::string reference;
    std::string resolvedPath;
    std::vector<unsigned char> pixels;
    std::size_t width = 0;
    std::size_t height = 0;
    std::size_t targetWidth = 0;
    std::size_t targetHeight = 0;
    AtlasRect placement;
    bool valid = false;
};

struct MaterialAtlasMapping {
    std::size_t sourceIndex = std::numeric_limits<std::size_t>::max();
    unsigned int materialIndex = 0;
    unsigned int textureIndex = 0;
    unsigned int uvChannel = 0;
    unsigned int targetUvChannel = 0;
    bool requiresNewUvChannel = false;
    aiTextureType textureType = aiTextureType_DIFFUSE;
    aiTextureMapMode mapU = aiTextureMapMode_Wrap;
    aiTextureMapMode mapV = aiTextureMapMode_Wrap;
    aiUVTransform transform;
    bool hasTransform = false;
    bool valid = false;
};

struct VertexKey {
    unsigned int originalIndex;
    std::uint32_t uBits;
    std::uint32_t vBits;

    bool operator<(const VertexKey &other) const {
        if (originalIndex != other.originalIndex) {
            return originalIndex < other.originalIndex;
        }
        if (uBits != other.uBits) {
            return uBits < other.uBits;
        }
        return vBits < other.vBits;
    }
};

std::uint32_t FloatBits(float value) {
    if (value == 0.0f) {
        value = 0.0f;
    }
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::string JoinPath(const std::string &directory, const std::string &filename) {
    if (directory.empty()) {
        return filename;
    }
    const char last = directory.back();
    if (last == '/' || last == '\\') {
        return directory + filename;
    }
    return directory + '/' + filename;
}

std::string NormalizeAtlasFilename(std::string filename) {
    if (filename.empty()) {
        filename = kDefaultAtlasFilename;
    }
    if (filename.size() < 4 ||
            filename.compare(filename.size() - 4, 4, ".png") != 0) {
        filename += ".png";
    }
    return filename;
}

std::string Basename(const std::string &path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool ResolveTexturePath(IOSystem *io, const std::string &rootPath,
        const std::string &reference, std::string &resolved) {
    if (io->Exists(reference)) {
        resolved = reference;
        return true;
    }

    const std::string rooted = rootPath + reference;
    if (io->Exists(rooted)) {
        resolved = rooted;
        return true;
    }

    const std::string rootedBasename = rootPath + Basename(reference);
    if (io->Exists(rootedBasename)) {
        resolved = rootedBasename;
        return true;
    }

    std::string normalized = reference;
    const char separator = io->getOsSeparator();
    if (separator != '\\') {
        std::replace(normalized.begin(), normalized.end(), '\\', separator);
    }
    if (normalized != reference && io->Exists(normalized)) {
        resolved = normalized;
        return true;
    }

    return false;
}

bool ReadFileBytes(IOSystem *io, const std::string &path,
        std::vector<unsigned char> &bytes) {
    IOStream *stream = io->Open(path);
    if (stream == nullptr) {
        return false;
    }

    const std::size_t size = stream->FileSize();
    bytes.resize(size);
    stream->Seek(0, aiOrigin_SET);
    const bool ok = size == 0 || stream->Read(bytes.data(), 1, size) == size;
    io->Close(stream);
    return ok;
}

bool ParseEmbeddedTextureIndex(const std::string &reference, unsigned int &index) {
    if (reference.empty() || reference[0] != '*') {
        return false;
    }
    if (reference.size() == 1) {
        return false;
    }

    char *end = nullptr;
    const unsigned long parsed = std::strtoul(reference.c_str() + 1, &end, 10);
    if (end == reference.c_str() + 1 || *end != '\0') {
        return false;
    }
    index = static_cast<unsigned int>(parsed);
    return true;
}

bool LoadEmbeddedTexture(const aiTexture *texture, SourceTexture &source) {
    if (texture == nullptr) {
        return false;
    }

    if (texture->mHeight == 0) {
        if (texture->mWidth == 0 || texture->pcData == nullptr) {
            return false;
        }
        int width = 0;
        int height = 0;
        int components = 0;
        unsigned char *decoded = stbi_load_from_memory(
                reinterpret_cast<const unsigned char *>(texture->pcData),
                static_cast<int>(texture->mWidth), &width, &height, &components, 4);
        if (decoded == nullptr || width <= 0 || height <= 0) {
            if (decoded != nullptr) {
                stbi_image_free(decoded);
            }
            return false;
        }

        source.width = static_cast<std::size_t>(width);
        source.height = static_cast<std::size_t>(height);
        source.pixels.assign(decoded, decoded + source.width * source.height * 4);
        stbi_image_free(decoded);
        source.valid = true;
        return true;
    }

    if (texture->mWidth == 0 || texture->mHeight == 0 || texture->pcData == nullptr) {
        return false;
    }

    source.width = static_cast<std::size_t>(texture->mWidth);
    source.height = static_cast<std::size_t>(texture->mHeight);
    source.pixels.resize(source.width * source.height * 4);

    const bool rgbaHint = std::strncmp(texture->achFormatHint, "rgba", 4) == 0;
    const bool argbHint = std::strncmp(texture->achFormatHint, "argb", 4) == 0;
    for (std::size_t i = 0; i < source.width * source.height; ++i) {
        const aiTexel &texel = texture->pcData[i];
        const std::size_t offset = i * 4;
        if (rgbaHint) {
            source.pixels[offset + 0] = texel.b;
            source.pixels[offset + 1] = texel.g;
            source.pixels[offset + 2] = texel.r;
            source.pixels[offset + 3] = texel.a;
        } else if (argbHint) {
            source.pixels[offset + 0] = texel.g;
            source.pixels[offset + 1] = texel.r;
            source.pixels[offset + 2] = texel.a;
            source.pixels[offset + 3] = texel.b;
        } else {
            source.pixels[offset + 0] = texel.r;
            source.pixels[offset + 1] = texel.g;
            source.pixels[offset + 2] = texel.b;
            source.pixels[offset + 3] = texel.a;
        }
    }
    source.valid = true;
    return true;
}

bool LoadExternalTexture(IOSystem *io, const std::string &path, SourceTexture &source) {
    std::vector<unsigned char> bytes;
    if (!ReadFileBytes(io, path, bytes) || bytes.empty()) {
        return false;
    }

    int width = 0;
    int height = 0;
    int components = 0;
    unsigned char *decoded = stbi_load_from_memory(bytes.data(),
            static_cast<int>(bytes.size()), &width, &height, &components, 4);
    if (decoded == nullptr || width <= 0 || height <= 0) {
        if (decoded != nullptr) {
            stbi_image_free(decoded);
        }
        return false;
    }

    source.width = static_cast<std::size_t>(width);
    source.height = static_cast<std::size_t>(height);
    source.pixels.assign(decoded, decoded + source.width * source.height * 4);
    stbi_image_free(decoded);
    source.valid = true;
    return true;
}

bool LoadSourceTexture(aiScene *scene, IOSystem *io, const std::string &rootPath,
        const std::string &reference, SourceTexture &source) {
    source.reference = reference;

    unsigned int embeddedIndex = 0;
    if (ParseEmbeddedTextureIndex(reference, embeddedIndex)) {
        if (embeddedIndex >= scene->mNumTextures || scene->mTextures == nullptr) {
            return false;
        }
        source.resolvedPath = reference;
        return LoadEmbeddedTexture(scene->mTextures[embeddedIndex], source);
    }

    if (!ResolveTexturePath(io, rootPath, reference, source.resolvedPath)) {
        return false;
    }
    return LoadExternalTexture(io, source.resolvedPath, source);
}

std::size_t ScaledDimension(std::size_t value, double scale) {
    const double scaled = std::ceil(static_cast<double>(value) * scale);
    return std::max<std::size_t>(1u, static_cast<std::size_t>(scaled));
}

bool TryPackTextures(const std::vector<SourceTexture> &sources,
        const std::vector<std::size_t> &sourceIndices, std::size_t maxAtlasSize,
        std::size_t padding, double scale, std::vector<AtlasRect> &rects,
        std::size_t &usedWidth, std::size_t &usedHeight) {
    rects.assign(sources.size(), AtlasRect());
    usedWidth = 0;
    usedHeight = 0;

    if (sourceIndices.empty()) {
        return true;
    }

    struct Item {
        std::size_t index;
        std::size_t width;
        std::size_t height;
    };

    std::vector<Item> items;
    items.reserve(sourceIndices.size());
    for (std::size_t index : sourceIndices) {
        const SourceTexture &source = sources[index];
        Item item;
        item.index = index;
        item.width = ScaledDimension(source.width, scale);
        item.height = ScaledDimension(source.height, scale);
        if (item.width + padding * 2 > maxAtlasSize ||
                item.height + padding * 2 > maxAtlasSize) {
            return false;
        }
        items.push_back(item);
    }

    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
        if (a.height != b.height) {
            return a.height > b.height;
        }
        return a.width > b.width;
    });

    std::size_t shelfX = 0;
    std::size_t shelfY = 0;
    std::size_t shelfHeight = 0;
    for (const Item &item : items) {
        const std::size_t blockWidth = item.width + padding * 2;
        const std::size_t blockHeight = item.height + padding * 2;

        if (shelfX + blockWidth > maxAtlasSize) {
            shelfY += shelfHeight;
            shelfX = 0;
            shelfHeight = 0;
        }
        if (shelfY + blockHeight > maxAtlasSize) {
            return false;
        }

        AtlasRect &rect = rects[item.index];
        rect.x = shelfX;
        rect.y = shelfY;
        rect.width = item.width;
        rect.height = item.height;

        shelfX += blockWidth;
        shelfHeight = std::max(shelfHeight, blockHeight);
        usedWidth = std::max(usedWidth, shelfX);
        usedHeight = std::max(usedHeight, shelfY + blockHeight);
    }

    return true;
}

bool PackTextures(std::vector<SourceTexture> &sources,
        const std::vector<std::size_t> &sourceIndices, std::size_t maxAtlasSize,
        std::size_t padding, std::vector<AtlasRect> &rects,
        std::size_t &atlasWidth, std::size_t &atlasHeight) {
    if (sourceIndices.empty()) {
        return false;
    }

    const double maxContentSize = static_cast<double>(maxAtlasSize) - 2.0 * static_cast<double>(padding);
    double scale = 1.0;
    double totalArea = 0.0;
    for (std::size_t index : sourceIndices) {
        const SourceTexture &source = sources[index];
        const double width = static_cast<double>(source.width);
        const double height = static_cast<double>(source.height);
        if (width > maxContentSize) {
            scale = std::min(scale, maxContentSize / width);
        }
        if (height > maxContentSize) {
            scale = std::min(scale, maxContentSize / height);
        }
        totalArea += (width + 2.0 * static_cast<double>(padding)) *
                (height + 2.0 * static_cast<double>(padding));
    }

    const double availableArea = maxContentSize * maxContentSize;
    if (totalArea > 0.0 && totalArea * scale * scale > availableArea) {
        scale = std::min(scale, std::sqrt(availableArea / totalArea) * 0.98);
    }

    while (scale > 1.0 / 4096.0) {
        if (TryPackTextures(sources, sourceIndices, maxAtlasSize, padding,
                    scale, rects, atlasWidth, atlasHeight)) {
            for (std::size_t index : sourceIndices) {
                SourceTexture &source = sources[index];
                source.targetWidth = ScaledDimension(source.width, scale);
                source.targetHeight = ScaledDimension(source.height, scale);
                source.placement.x = rects[index].x + padding;
                source.placement.y = rects[index].y + padding;
                source.placement.width = source.targetWidth;
                source.placement.height = source.targetHeight;
            }
            return true;
        }
        scale *= 0.9;
    }

    return false;
}

unsigned char BilinearChannel(const SourceTexture &source, std::size_t targetWidth,
        std::size_t targetHeight, std::size_t x, std::size_t y, unsigned int channel) {
    const double sourceX = ((static_cast<double>(x) + 0.5) *
            static_cast<double>(source.width) / static_cast<double>(targetWidth)) - 0.5;
    const double sourceY = ((static_cast<double>(y) + 0.5) *
            static_cast<double>(source.height) / static_cast<double>(targetHeight)) - 0.5;

    const std::size_t x0 = static_cast<std::size_t>(
            std::max(0.0, std::floor(sourceX)));
    const std::size_t y0 = static_cast<std::size_t>(
            std::max(0.0, std::floor(sourceY)));
    const std::size_t x1 = std::min(x0 + 1, source.width - 1);
    const std::size_t y1 = std::min(y0 + 1, source.height - 1);
    const double fx = std::max(0.0, sourceX - static_cast<double>(x0));
    const double fy = std::max(0.0, sourceY - static_cast<double>(y0));

    const auto sample = [&](std::size_t sx, std::size_t sy) {
        return static_cast<double>(source.pixels[(sy * source.width + sx) * 4 + channel]);
    };

    const double top = sample(x0, y0) * (1.0 - fx) + sample(x1, y0) * fx;
    const double bottom = sample(x0, y1) * (1.0 - fx) + sample(x1, y1) * fx;
    const double value = top * (1.0 - fy) + bottom * fy;
    return static_cast<unsigned char>(std::clamp(value + 0.5, 0.0, 255.0));
}

void BuildAtlasImage(std::vector<SourceTexture> &sources,
        const std::vector<std::size_t> &sourceIndices, std::size_t padding,
        std::size_t atlasWidth, std::size_t atlasHeight,
        std::vector<unsigned char> &atlas) {
    atlas.assign(atlasWidth * atlasHeight * 4, 0);

    for (std::size_t index : sourceIndices) {
        SourceTexture &source = sources[index];
        const std::size_t blockWidth = source.placement.width + padding * 2;
        const std::size_t blockHeight = source.placement.height + padding * 2;
        for (std::size_t y = 0; y < blockHeight; ++y) {
            const std::size_t sourceY = std::min(
                    y < padding ? 0 : y - padding, source.placement.height - 1);
            for (std::size_t x = 0; x < blockWidth; ++x) {
                const std::size_t sourceX = std::min(
                        x < padding ? 0 : x - padding, source.placement.width - 1);
                const std::size_t destX = source.placement.x - padding + x;
                const std::size_t destY = source.placement.y - padding + y;
                const std::size_t destOffset = (destY * atlasWidth + destX) * 4;
                for (unsigned int channel = 0; channel < 4; ++channel) {
                    atlas[destOffset + channel] = BilinearChannel(source,
                            source.placement.width, source.placement.height,
                            sourceX, sourceY, channel);
                }
            }
        }
    }
}

void AppendU32(std::vector<unsigned char> &output, std::uint32_t value) {
    output.push_back(static_cast<unsigned char>((value >> 24) & 0xff));
    output.push_back(static_cast<unsigned char>((value >> 16) & 0xff));
    output.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
    output.push_back(static_cast<unsigned char>(value & 0xff));
}

void AppendChunk(std::vector<unsigned char> &output, const char *type,
        const unsigned char *data, std::uint32_t size) {
    AppendU32(output, size);
    const std::size_t typeOffset = output.size();
    output.insert(output.end(), type, type + 4);
    if (size != 0) {
        output.insert(output.end(), data, data + size);
    }

    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef *>(output.data() + typeOffset), 4);
    if (size != 0) {
        crc = crc32(crc, data, size);
    }
    AppendU32(output, static_cast<std::uint32_t>(crc));
}

bool WritePng(IOSystem *io, const std::string &path, std::size_t width,
        std::size_t height, const std::vector<unsigned char> &rgba) {
    if (width == 0 || height == 0 || width > 0xffffffffu || height > 0xffffffffu) {
        return false;
    }

    std::vector<unsigned char> raw;
    raw.reserve(height * (width * 4 + 1));
    for (std::size_t y = 0; y < height; ++y) {
        raw.push_back(0);
        const std::size_t offset = y * width * 4;
        raw.insert(raw.end(), rgba.begin() + static_cast<std::ptrdiff_t>(offset),
                rgba.begin() + static_cast<std::ptrdiff_t>(offset + width * 4));
    }

    std::vector<unsigned char> compressed(compressBound(static_cast<uLong>(raw.size())));
    uLongf compressedSize = static_cast<uLongf>(compressed.size());
    if (compress2(compressed.data(), &compressedSize, raw.data(),
                static_cast<uLong>(raw.size()), Z_BEST_COMPRESSION) != Z_OK) {
        return false;
    }
    compressed.resize(static_cast<std::size_t>(compressedSize));

    std::vector<unsigned char> png;
    static const unsigned char signature[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    png.insert(png.end(), signature, signature + 8);

    unsigned char ihdr[13];
    ihdr[0] = static_cast<unsigned char>((width >> 24) & 0xff);
    ihdr[1] = static_cast<unsigned char>((width >> 16) & 0xff);
    ihdr[2] = static_cast<unsigned char>((width >> 8) & 0xff);
    ihdr[3] = static_cast<unsigned char>(width & 0xff);
    ihdr[4] = static_cast<unsigned char>((height >> 24) & 0xff);
    ihdr[5] = static_cast<unsigned char>((height >> 16) & 0xff);
    ihdr[6] = static_cast<unsigned char>((height >> 8) & 0xff);
    ihdr[7] = static_cast<unsigned char>(height & 0xff);
    ihdr[8] = 8;
    ihdr[9] = 6;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    AppendChunk(png, "IHDR", ihdr, sizeof(ihdr));

    const unsigned char srgbIntent = 0;
    AppendChunk(png, "sRGB", &srgbIntent, 1);
    AppendChunk(png, "IDAT", compressed.data(), static_cast<std::uint32_t>(compressed.size()));
    AppendChunk(png, "IEND", nullptr, 0);

    IOStream *stream = io->Open(path, "wb");
    if (stream == nullptr) {
        return false;
    }
    const bool ok = stream->Write(png.data(), 1, png.size()) == png.size();
    stream->Flush();
    io->Close(stream);
    return ok;
}

float WrapCoordinate(float value, aiTextureMapMode mode) {
    if (!std::isfinite(value)) {
        return 0.0f;
    }

    switch (mode) {
    case aiTextureMapMode_Clamp:
    case aiTextureMapMode_Decal:
        return std::clamp(value, 0.0f, 1.0f);
    case aiTextureMapMode_Mirror: {
        float wrapped = std::fmod(value, 2.0f);
        if (wrapped < 0.0f) {
            wrapped += 2.0f;
        }
        return wrapped > 1.0f ? 2.0f - wrapped : wrapped;
    }
    case aiTextureMapMode_Wrap:
    default:
        value -= std::floor(value);
        return std::clamp(value, 0.0f, 1.0f);
    }
}

void MapTextureCoordinate(const aiVector3D &source,
        const MaterialAtlasMapping &mapping, const SourceTexture &texture,
        std::size_t atlasWidth, std::size_t atlasHeight, float &outU, float &outV) {
    float u = source.x;
    float v = source.y;

    if (mapping.hasTransform) {
        const float centeredU = u - 0.5f;
        const float centeredV = v - 0.5f;
        const float cosValue = std::cos(mapping.transform.mRotation);
        const float sinValue = std::sin(mapping.transform.mRotation);
        const float rotatedU = centeredU * cosValue - centeredV * sinValue;
        const float rotatedV = centeredU * sinValue + centeredV * cosValue;
        u = rotatedU * mapping.transform.mScaling.x + 0.5f + mapping.transform.mTranslation.x;
        v = rotatedV * mapping.transform.mScaling.y + 0.5f + mapping.transform.mTranslation.y;
    }

    u = WrapCoordinate(u, mapping.mapU);
    v = WrapCoordinate(v, mapping.mapV);

    outU = (static_cast<float>(texture.placement.x) + u *
            static_cast<float>(texture.placement.width)) / static_cast<float>(atlasWidth);
    outV = (static_cast<float>(texture.placement.y) + v *
            static_cast<float>(texture.placement.height)) / static_cast<float>(atlasHeight);
}

bool HasUsableTextureCoordinates(const aiMesh *mesh,
        const MaterialAtlasMapping &mapping) {
    if (mesh == nullptr || mesh->mVertices == nullptr) {
        return false;
    }
    unsigned int sourceUvChannel = mapping.uvChannel;
    if (sourceUvChannel >= AI_MAX_NUMBER_OF_TEXTURECOORDS ||
            mesh->mTextureCoords[sourceUvChannel] == nullptr) {
        sourceUvChannel = 0;
    }
    if (mesh->mTextureCoords[sourceUvChannel] == nullptr ||
            mesh->mNumUVComponents[sourceUvChannel] > 2) {
        return false;
    }
    const unsigned int targetUvChannel = mapping.targetUvChannel;
    if (targetUvChannel >= AI_MAX_NUMBER_OF_TEXTURECOORDS) {
        return false;
    }
    for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex) {
        const aiFace &face = mesh->mFaces[faceIndex];
        for (unsigned int corner = 0; corner < face.mNumIndices; ++corner) {
            if (face.mIndices[corner] >= mesh->mNumVertices) {
                return false;
            }
        }
    }
    return true;
}

bool HasOtherTextureOnChannel(const aiMaterial *material,
        const MaterialAtlasMapping &mapping) {
    for (unsigned int typeIndex = 1;
            typeIndex <= static_cast<unsigned int>(AI_TEXTURE_TYPE_MAX); ++typeIndex) {
        const aiTextureType type = static_cast<aiTextureType>(typeIndex);
        const unsigned int count = material->GetTextureCount(type);
        for (unsigned int index = 0; index < count; ++index) {
            aiString path;
            aiTextureMapping textureMapping = aiTextureMapping_UV;
            unsigned int uvIndex = 0;
            if (material->GetTexture(type, index, &path, &textureMapping,
                        &uvIndex) != AI_SUCCESS ||
                    textureMapping != aiTextureMapping_UV ||
                    uvIndex != mapping.uvChannel) {
                continue;
            }
            if (type != mapping.textureType || index != mapping.textureIndex) {
                return true;
            }
        }
    }
    return false;
}

bool MaterialReferencesUvChannel(const aiMaterial *material,
        unsigned int channel) {
    for (unsigned int typeIndex = 1;
            typeIndex <= static_cast<unsigned int>(AI_TEXTURE_TYPE_MAX); ++typeIndex) {
        const aiTextureType type = static_cast<aiTextureType>(typeIndex);
        const unsigned int count = material->GetTextureCount(type);
        for (unsigned int index = 0; index < count; ++index) {
            aiString path;
            aiTextureMapping textureMapping = aiTextureMapping_UV;
            unsigned int uvIndex = 0;
            if (material->GetTexture(type, index, &path, &textureMapping,
                        &uvIndex) == AI_SUCCESS &&
                    textureMapping == aiTextureMapping_UV &&
                    uvIndex == channel) {
                return true;
            }
        }
    }
    return false;
}

bool RemapMesh(aiMesh *mesh, const MaterialAtlasMapping &mapping,
        const SourceTexture &texture, std::size_t atlasWidth,
        std::size_t atlasHeight, std::unique_ptr<aiMesh> &result) {
    if (mesh == nullptr) {
        return false;
    }

    unsigned int sourceUvChannel = mapping.uvChannel;
    if (sourceUvChannel >= AI_MAX_NUMBER_OF_TEXTURECOORDS ||
            mesh->mTextureCoords[sourceUvChannel] == nullptr) {
        sourceUvChannel = 0;
    }
    if (mesh->mTextureCoords[sourceUvChannel] == nullptr ||
            mesh->mNumUVComponents[sourceUvChannel] > 2) {
        return false;
    }
    const unsigned int targetUvChannel = mapping.targetUvChannel;
    if (targetUvChannel >= AI_MAX_NUMBER_OF_TEXTURECOORDS) {
        return false;
    }

    std::map<VertexKey, unsigned int> newVertexMap;
    std::vector<unsigned int> newVertexOriginalIndex;
    std::vector<aiVector3D> positions;
    std::vector<aiVector3D> normals;
    std::vector<aiVector3D> tangents;
    std::vector<aiVector3D> bitangents;
    std::vector<aiColor4D> colors[AI_MAX_NUMBER_OF_COLOR_SETS];
    std::vector<aiVector3D> textureCoords[AI_MAX_NUMBER_OF_TEXTURECOORDS];
    std::vector<std::vector<std::pair<unsigned int, ai_real>>> boneInfluences(mesh->mNumVertices);
    std::vector<unsigned int> faceIndices;
    std::vector<unsigned int> faceStarts;
    faceStarts.reserve(mesh->mNumFaces + 1);

    if (mesh->mVertices != nullptr) {
        positions.reserve(mesh->mNumVertices);
    }
    if (mesh->mNormals != nullptr) {
        normals.reserve(mesh->mNumVertices);
    }
    if (mesh->mTangents != nullptr) {
        tangents.reserve(mesh->mNumVertices);
    }
    if (mesh->mBitangents != nullptr) {
        bitangents.reserve(mesh->mNumVertices);
    }
    for (unsigned int channel = 0; channel < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++channel) {
        if (mesh->mTextureCoords[channel] != nullptr) {
            textureCoords[channel].reserve(mesh->mNumVertices);
        }
    }
    for (unsigned int channel = 0; channel < AI_MAX_NUMBER_OF_COLOR_SETS; ++channel) {
        if (mesh->mColors[channel] != nullptr) {
            colors[channel].reserve(mesh->mNumVertices);
        }
    }

    for (unsigned int boneIndex = 0; boneIndex < mesh->mNumBones; ++boneIndex) {
        const aiBone *bone = mesh->mBones[boneIndex];
        if (bone == nullptr) {
            continue;
        }
        for (unsigned int weightIndex = 0; weightIndex < bone->mNumWeights; ++weightIndex) {
            const aiVertexWeight &weight = bone->mWeights[weightIndex];
            if (weight.mVertexId < mesh->mNumVertices) {
                boneInfluences[weight.mVertexId].emplace_back(boneIndex, weight.mWeight);
            }
        }
    }

    faceStarts.push_back(0);
    for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex) {
        const aiFace &face = mesh->mFaces[faceIndex];
        faceIndices.reserve(faceIndices.size() + face.mNumIndices);
        for (unsigned int corner = 0; corner < face.mNumIndices; ++corner) {
            const unsigned int originalIndex = face.mIndices[corner];
            if (originalIndex >= mesh->mNumVertices) {
                return false;
            }

            float atlasU = 0.0f;
            float atlasV = 0.0f;
            MapTextureCoordinate(mesh->mTextureCoords[sourceUvChannel][originalIndex],
                    mapping, texture, atlasWidth, atlasHeight, atlasU, atlasV);

            VertexKey key;
            key.originalIndex = originalIndex;
            key.uBits = FloatBits(atlasU);
            key.vBits = FloatBits(atlasV);
            const auto found = newVertexMap.find(key);
            if (found != newVertexMap.end()) {
                faceIndices.push_back(found->second);
                continue;
            }

            const unsigned int newIndex = static_cast<unsigned int>(positions.size());
            newVertexMap.emplace(key, newIndex);
            newVertexOriginalIndex.push_back(originalIndex);

            if (mesh->mVertices != nullptr) {
                positions.push_back(mesh->mVertices[originalIndex]);
            }
            if (mesh->mNormals != nullptr) {
                normals.push_back(mesh->mNormals[originalIndex]);
            }
            if (mesh->mTangents != nullptr) {
                tangents.push_back(mesh->mTangents[originalIndex]);
            }
            if (mesh->mBitangents != nullptr) {
                bitangents.push_back(mesh->mBitangents[originalIndex]);
            }
            for (unsigned int channel = 0; channel < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++channel) {
                if (mesh->mTextureCoords[channel] != nullptr) {
                    textureCoords[channel].push_back(mesh->mTextureCoords[channel][originalIndex]);
                }
            }
            if (mesh->mTextureCoords[targetUvChannel] == nullptr) {
                textureCoords[targetUvChannel].push_back(
                        aiVector3D(atlasU, atlasV, 0.0f));
            } else {
                textureCoords[targetUvChannel].back().x = atlasU;
                textureCoords[targetUvChannel].back().y = atlasV;
            }
            for (unsigned int channel = 0; channel < AI_MAX_NUMBER_OF_COLOR_SETS; ++channel) {
                if (mesh->mColors[channel] != nullptr) {
                    colors[channel].push_back(mesh->mColors[channel][originalIndex]);
                }
            }

            faceIndices.push_back(newIndex);
        }
        faceStarts.push_back(static_cast<unsigned int>(faceIndices.size()));
    }

    result.reset(new aiMesh());
    aiMesh *out = result.get();
    out->mName = mesh->mName;
    out->mPrimitiveTypes = mesh->mPrimitiveTypes;
    out->mMaterialIndex = mesh->mMaterialIndex;
    out->mMethod = mesh->mMethod;
    out->mAABB = mesh->mAABB;
    out->mNumVertices = static_cast<unsigned int>(positions.size());

    if (!positions.empty()) {
        out->mVertices = new aiVector3D[positions.size()];
        std::copy(positions.begin(), positions.end(), out->mVertices);
    }
    if (!normals.empty()) {
        out->mNormals = new aiVector3D[normals.size()];
        std::copy(normals.begin(), normals.end(), out->mNormals);
    }
    if (!tangents.empty()) {
        out->mTangents = new aiVector3D[tangents.size()];
        std::copy(tangents.begin(), tangents.end(), out->mTangents);
    }
    if (!bitangents.empty()) {
        out->mBitangents = new aiVector3D[bitangents.size()];
        std::copy(bitangents.begin(), bitangents.end(), out->mBitangents);
    }
    for (unsigned int channel = 0; channel < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++channel) {
        out->mNumUVComponents[channel] = mesh->mTextureCoords[channel] != nullptr
                ? mesh->mNumUVComponents[channel]
                : 0;
        if (channel == targetUvChannel) {
            out->mNumUVComponents[channel] = 2;
        }
        if (!textureCoords[channel].empty()) {
            out->mTextureCoords[channel] = new aiVector3D[textureCoords[channel].size()];
            std::copy(textureCoords[channel].begin(), textureCoords[channel].end(),
                    out->mTextureCoords[channel]);
        }
    }
    for (unsigned int channel = 0; channel < AI_MAX_NUMBER_OF_COLOR_SETS; ++channel) {
        if (!colors[channel].empty()) {
            out->mColors[channel] = new aiColor4D[colors[channel].size()];
            std::copy(colors[channel].begin(), colors[channel].end(), out->mColors[channel]);
        }
    }

    out->mNumFaces = mesh->mNumFaces;
    out->mFaces = new aiFace[mesh->mNumFaces];
    for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex) {
        out->mFaces[faceIndex].mNumIndices =
                faceStarts[faceIndex + 1] - faceStarts[faceIndex];
        out->mFaces[faceIndex].mIndices = new unsigned int[out->mFaces[faceIndex].mNumIndices];
        std::copy(faceIndices.begin() + faceStarts[faceIndex],
                faceIndices.begin() + faceStarts[faceIndex + 1],
                out->mFaces[faceIndex].mIndices);
    }

    out->mNumBones = mesh->mNumBones;
    if (mesh->mNumBones != 0) {
        out->mBones = new aiBone *[mesh->mNumBones];
        for (unsigned int boneIndex = 0; boneIndex < mesh->mNumBones; ++boneIndex) {
            const aiBone *sourceBone = mesh->mBones[boneIndex];
            aiBone *newBone = new aiBone();
            out->mBones[boneIndex] = newBone;
            if (sourceBone == nullptr) {
                continue;
            }
            newBone->mName = sourceBone->mName;
            newBone->mOffsetMatrix = sourceBone->mOffsetMatrix;
#ifndef ASSIMP_BUILD_NO_ARMATUREPOPULATE_PROCESS
            newBone->mArmature = sourceBone->mArmature;
            newBone->mNode = sourceBone->mNode;
#endif

            std::vector<aiVertexWeight> weights;
            for (std::size_t newVertex = 0; newVertex < newVertexOriginalIndex.size();
                    ++newVertex) {
                const unsigned int originalIndex = newVertexOriginalIndex[newVertex];
                for (const auto &influence : boneInfluences[originalIndex]) {
                    if (influence.first == boneIndex) {
                        weights.emplace_back(static_cast<unsigned int>(newVertex),
                                influence.second);
                    }
                }
            }
            newBone->mNumWeights = static_cast<unsigned int>(weights.size());
            if (!weights.empty()) {
                newBone->mWeights = new aiVertexWeight[weights.size()];
                std::copy(weights.begin(), weights.end(), newBone->mWeights);
            }
        }
    }

    if (mesh->mTextureCoordsNames != nullptr) {
        out->mTextureCoordsNames = new aiString *[AI_MAX_NUMBER_OF_TEXTURECOORDS];
        for (unsigned int channel = 0; channel < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++channel) {
            out->mTextureCoordsNames[channel] = nullptr;
            if (mesh->mTextureCoordsNames[channel] != nullptr) {
                out->mTextureCoordsNames[channel] =
                        new aiString(*mesh->mTextureCoordsNames[channel]);
            }
        }
    }

    out->mNumAnimMeshes = mesh->mNumAnimMeshes;
    if (mesh->mNumAnimMeshes != 0) {
        out->mAnimMeshes = new aiAnimMesh *[mesh->mNumAnimMeshes];
        for (unsigned int animIndex = 0; animIndex < mesh->mNumAnimMeshes; ++animIndex) {
            const aiAnimMesh *sourceAnim = mesh->mAnimMeshes[animIndex];
            aiAnimMesh *newAnim = new aiAnimMesh();
            out->mAnimMeshes[animIndex] = newAnim;
            if (sourceAnim == nullptr) {
                continue;
            }
            newAnim->mName = sourceAnim->mName;
            newAnim->mWeight = sourceAnim->mWeight;
            newAnim->mNumVertices = out->mNumVertices;

            const auto copyAnimVectorArray = [&](const aiVector3D *source,
                    aiVector3D *&destination) {
                if (source == nullptr) {
                    return;
                }
                destination = new aiVector3D[newVertexOriginalIndex.size()];
                for (std::size_t i = 0; i < newVertexOriginalIndex.size(); ++i) {
                    destination[i] = source[newVertexOriginalIndex[i]];
                }
            };
            copyAnimVectorArray(sourceAnim->mVertices, newAnim->mVertices);
            copyAnimVectorArray(sourceAnim->mNormals, newAnim->mNormals);
            copyAnimVectorArray(sourceAnim->mTangents, newAnim->mTangents);
            copyAnimVectorArray(sourceAnim->mBitangents, newAnim->mBitangents);
            for (unsigned int channel = 0; channel < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++channel) {
                copyAnimVectorArray(sourceAnim->mTextureCoords[channel],
                        newAnim->mTextureCoords[channel]);
                if (channel == targetUvChannel &&
                        newAnim->mTextureCoords[channel] != nullptr &&
                        sourceAnim->mTextureCoords[sourceUvChannel] != nullptr) {
                    for (std::size_t i = 0; i < newVertexOriginalIndex.size(); ++i) {
                        float atlasU = 0.0f;
                        float atlasV = 0.0f;
                        MapTextureCoordinate(
                                sourceAnim->mTextureCoords[sourceUvChannel][newVertexOriginalIndex[i]],
                                mapping, texture, atlasWidth, atlasHeight, atlasU, atlasV);
                        newAnim->mTextureCoords[channel][i].x = atlasU;
                        newAnim->mTextureCoords[channel][i].y = atlasV;
                    }
                }
            }
            for (unsigned int channel = 0; channel < AI_MAX_NUMBER_OF_COLOR_SETS; ++channel) {
                if (sourceAnim->mColors[channel] == nullptr) {
                    continue;
                }
                newAnim->mColors[channel] =
                        new aiColor4D[newVertexOriginalIndex.size()];
                for (std::size_t i = 0; i < newVertexOriginalIndex.size(); ++i) {
                    newAnim->mColors[channel][i] =
                            sourceAnim->mColors[channel][newVertexOriginalIndex[i]];
                }
            }
        }
    }

    return true;
}

} // namespace

BakeTextureAtlasProcess::BakeTextureAtlasProcess() :
        BaseProcess(),
        mMaxAtlasSize(kDefaultMaxAtlasSize),
        mPadding(kDefaultPadding),
        mOutputDirectory(),
        mAtlasFilename(kDefaultAtlasFilename),
        mRootPath(),
        mIOHandler(nullptr) {
    // empty
}

bool BakeTextureAtlasProcess::IsActive(aiPostProcessFlags pFlags) const {
    return (pFlags & aiProcess_BakeTextureAtlas) != 0;
}

void BakeTextureAtlasProcess::SetupProperties(const Importer *pImp) {
    const int configuredSize = pImp->GetPropertyInteger(
            AI_CONFIG_PP_BTA_MAX_ATLAS_SIZE, static_cast<int>(kDefaultMaxAtlasSize));
    mMaxAtlasSize = configuredSize > 0
            ? static_cast<std::size_t>(configuredSize)
            : kDefaultMaxAtlasSize;
    mMaxAtlasSize = std::min<std::size_t>(mMaxAtlasSize, 16384);

    const int configuredPadding = pImp->GetPropertyInteger(
            AI_CONFIG_PP_BTA_PADDING, static_cast<int>(kDefaultPadding));
    mPadding = configuredPadding > 0
            ? static_cast<std::size_t>(configuredPadding)
            : kDefaultPadding;
    mPadding = std::min<std::size_t>(mPadding, 64);

    mOutputDirectory = pImp->GetPropertyString(AI_CONFIG_PP_BTA_OUTPUT_DIR, "");
    mAtlasFilename = NormalizeAtlasFilename(
            pImp->GetPropertyString(AI_CONFIG_PP_BTA_ATLAS_FILENAME,
                    kDefaultAtlasFilename));

    mRootPath = pImp->GetPropertyString("sourceFilePath");
    mRootPath = mRootPath.substr(0, mRootPath.find_last_of("\\/") + 1u);
    if (mOutputDirectory.empty()) {
        mOutputDirectory = mRootPath;
    }
    mIOHandler = pImp->GetIOHandler();
}

void BakeTextureAtlasProcess::Execute(aiScene *pScene) {
    if (pScene == nullptr || mIOHandler == nullptr || pScene->mNumMaterials == 0) {
        return;
    }

    std::vector<SourceTexture> sources;
    std::unordered_map<std::string, std::size_t> sourceByKey;
    std::vector<MaterialAtlasMapping> mappings(pScene->mNumMaterials);

    const auto addSource = [&](const std::string &reference) -> std::size_t {
        std::string key;
        unsigned int embeddedIndex = 0;
        if (ParseEmbeddedTextureIndex(reference, embeddedIndex)) {
            key = reference;
        } else {
            std::string resolved;
            key = ResolveTexturePath(mIOHandler, mRootPath, reference, resolved)
                    ? resolved
                    : std::string("missing:") + reference;
        }

        const auto found = sourceByKey.find(key);
        if (found != sourceByKey.end()) {
            return found->second;
        }

        const std::size_t index = sources.size();
        sourceByKey.emplace(key, index);
        sources.emplace_back();
        if (!LoadSourceTexture(pScene, mIOHandler, mRootPath, reference, sources.back())) {
            ASSIMP_LOG_WARN("BakeTextureAtlasProcess: unable to load texture ", reference);
        }
        return index;
    };

    for (unsigned int materialIndex = 0; materialIndex < pScene->mNumMaterials;
            ++materialIndex) {
        aiMaterial *material = pScene->mMaterials[materialIndex];
        if (material == nullptr) {
            continue;
        }

        aiString alreadyBakedSource;
        if (material->Get(AI_MATKEY_PP_BTA_SOURCE_TEXTURE,
                    alreadyBakedSource) == AI_SUCCESS) {
            continue;
        }

        aiTextureType textureType = aiTextureType_BASE_COLOR;
        unsigned int textureCount = material->GetTextureCount(textureType);
        if (textureCount == 0) {
            textureType = aiTextureType_DIFFUSE;
            textureCount = material->GetTextureCount(textureType);
        }
        if (textureCount == 0) {
            continue;
        }

        MaterialAtlasMapping &mapping = mappings[materialIndex];
        mapping.materialIndex = materialIndex;
        mapping.textureType = textureType;

        for (unsigned int textureIndex = 0; textureIndex < textureCount; ++textureIndex) {
            aiString texturePath;
            if (material->GetTexture(textureType, textureIndex, &texturePath) != AI_SUCCESS) {
                continue;
            }
            const std::size_t sourceIndex = addSource(texturePath.C_Str());
            if (textureIndex == 0) {
                mapping.textureIndex = 0;
                mapping.sourceIndex = sourceIndex;
            }
        }

        if (mapping.sourceIndex == std::numeric_limits<std::size_t>::max()) {
            continue;
        }

        aiString texturePath;
        aiTextureMapping textureMapping = aiTextureMapping_UV;
        unsigned int uvIndex = 0;
        aiTextureMapMode mapModes[2] = { aiTextureMapMode_Wrap, aiTextureMapMode_Wrap };
        if (material->GetTexture(mapping.textureType, 0, &texturePath,
                &textureMapping, &uvIndex, nullptr, nullptr, mapModes) != AI_SUCCESS ||
                textureMapping != aiTextureMapping_UV) {
            continue;
        }
        mapping.uvChannel = std::min(uvIndex, AI_MAX_NUMBER_OF_TEXTURECOORDS - 1u);
        mapping.targetUvChannel = mapping.uvChannel;
        mapping.mapU = mapModes[0];
        mapping.mapV = mapModes[1];

        aiUVTransform transform;
        if (material->Get(AI_MATKEY_UVTRANSFORM(mapping.textureType, 0),
                    transform) == AI_SUCCESS) {
            mapping.transform = transform;
            mapping.hasTransform = true;
        }
        mapping.requiresNewUvChannel =
                HasOtherTextureOnChannel(material, mapping);
        mapping.valid = true;
    }

    std::vector<std::size_t> packedSourceIndices;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        if (sources[index].valid) {
            packedSourceIndices.push_back(index);
        }
    }
    if (packedSourceIndices.empty()) {
        return;
    }

    const std::string atlasPath = JoinPath(mOutputDirectory, mAtlasFilename);
    std::vector<AtlasRect> placements;
    std::size_t atlasWidth = 0;
    std::size_t atlasHeight = 0;
    if (!PackTextures(sources, packedSourceIndices, mMaxAtlasSize, mPadding,
                placements, atlasWidth, atlasHeight)) {
        throw DeadlyImportError(
                "BakeTextureAtlasProcess: unable to pack textures into a ",
                mMaxAtlasSize, "x", mMaxAtlasSize, " atlas");
    }

    std::vector<unsigned char> atlas;
    BuildAtlasImage(sources, packedSourceIndices, mPadding,
            atlasWidth, atlasHeight, atlas);
    if (!WritePng(mIOHandler, atlasPath, atlasWidth, atlasHeight, atlas)) {
        throw DeadlyImportError(
                "BakeTextureAtlasProcess: unable to write atlas to ", atlasPath);
    }

    std::vector<bool> materialRemappable(pScene->mNumMaterials, false);
    for (unsigned int materialIndex = 0; materialIndex < pScene->mNumMaterials;
            ++materialIndex) {
        const MaterialAtlasMapping &mapping = mappings[materialIndex];
        if (!mapping.valid || mapping.sourceIndex >= sources.size() ||
                !sources[mapping.sourceIndex].valid) {
            continue;
        }

        bool hasMesh = false;
        bool allMeshesUsable = true;
        for (unsigned int meshIndex = 0; meshIndex < pScene->mNumMeshes; ++meshIndex) {
            const aiMesh *mesh = pScene->mMeshes[meshIndex];
            if (mesh == nullptr || mesh->mMaterialIndex != materialIndex) {
                continue;
            }
            hasMesh = true;
            if (!HasUsableTextureCoordinates(mesh, mapping)) {
                allMeshesUsable = false;
                break;
            }
        }
        materialRemappable[materialIndex] = hasMesh && allMeshesUsable;
    }

    for (unsigned int materialIndex = 0; materialIndex < pScene->mNumMaterials;
            ++materialIndex) {
        MaterialAtlasMapping &mapping = mappings[materialIndex];
        if (!materialRemappable[materialIndex] || !mapping.requiresNewUvChannel) {
            continue;
        }

        bool foundChannel = false;
        for (unsigned int candidate = 0;
                candidate < AI_MAX_NUMBER_OF_TEXTURECOORDS && !foundChannel;
                ++candidate) {
            if (candidate == mapping.uvChannel) {
                continue;
            }
            if (MaterialReferencesUvChannel(pScene->mMaterials[materialIndex],
                        candidate)) {
                continue;
            }
            bool available = true;
            for (unsigned int meshIndex = 0; meshIndex < pScene->mNumMeshes;
                    ++meshIndex) {
                const aiMesh *mesh = pScene->mMeshes[meshIndex];
                if (mesh != nullptr && mesh->mMaterialIndex == materialIndex &&
                        mesh->mTextureCoords[candidate] != nullptr) {
                    available = false;
                    break;
                }
            }
            if (available) {
                mapping.targetUvChannel = candidate;
                foundChannel = true;
            }
        }
        if (!foundChannel) {
            throw DeadlyImportError(
                    "BakeTextureAtlasProcess: no free UV channel for material ",
                    materialIndex);
        }
    }

    for (unsigned int meshIndex = 0; meshIndex < pScene->mNumMeshes; ++meshIndex) {
        aiMesh *mesh = pScene->mMeshes[meshIndex];
        if (mesh == nullptr || mesh->mMaterialIndex >= mappings.size()) {
            continue;
        }
        const MaterialAtlasMapping &mapping = mappings[mesh->mMaterialIndex];
        if (!materialRemappable[mesh->mMaterialIndex]) {
            continue;
        }

        std::unique_ptr<aiMesh> remapped;
        if (!RemapMesh(mesh, mapping, sources[mapping.sourceIndex],
                    atlasWidth, atlasHeight, remapped)) {
            throw DeadlyImportError(
                    "BakeTextureAtlasProcess: failed to remap mesh ", meshIndex);
        }
        pScene->mMeshes[meshIndex] = remapped.release();
        delete mesh;
    }

    for (unsigned int materialIndex = 0; materialIndex < pScene->mNumMaterials;
            ++materialIndex) {
        const MaterialAtlasMapping &mapping = mappings[materialIndex];
        if (!materialRemappable[materialIndex]) {
            continue;
        }

        aiMaterial *material = pScene->mMaterials[materialIndex];
        aiString atlasReference(atlasPath);
        aiString originalReference(sources[mapping.sourceIndex].reference);
        const int isSrgb = 1;
        const int clampMode = aiTextureMapMode_Clamp;
        const int uvChannel = static_cast<int>(mapping.targetUvChannel);
        const SourceTexture &texture = sources[mapping.sourceIndex];
        float rect[4] = {
            static_cast<float>(texture.placement.x) / static_cast<float>(atlasWidth),
            static_cast<float>(texture.placement.y) / static_cast<float>(atlasHeight),
            static_cast<float>(texture.placement.width) / static_cast<float>(atlasWidth),
            static_cast<float>(texture.placement.height) / static_cast<float>(atlasHeight)
        };

        material->AddProperty(&atlasReference,
                AI_MATKEY_TEXTURE(mapping.textureType, mapping.textureIndex));
        material->AddProperty(&originalReference, AI_MATKEY_PP_BTA_SOURCE_TEXTURE);
        material->AddProperty(rect, 4, AI_MATKEY_PP_BTA_UV_RECT);
        material->AddProperty(&isSrgb, 1, AI_MATKEY_PP_BTA_SRGB);
        material->AddProperty(&clampMode, 1,
                AI_MATKEY_MAPPINGMODE_U(mapping.textureType, mapping.textureIndex));
        material->AddProperty(&clampMode, 1,
                AI_MATKEY_MAPPINGMODE_V(mapping.textureType, mapping.textureIndex));
        material->RemoveProperty(_AI_MATKEY_UVTRANSFORM_BASE,
                mapping.textureType, mapping.textureIndex);
        material->AddProperty(&uvChannel, 1,
                AI_MATKEY_UVWSRC(mapping.textureType, mapping.textureIndex));
    }

    ASSIMP_LOG_INFO("BakeTextureAtlasProcess: wrote ", atlasPath, " (",
            atlasWidth, "x", atlasHeight, ")");
}

} // namespace Assimp

#endif // ASSIMP_BUILD_NO_BAKETEXTUREATLAS_PROCESS
