#include "UnitTestPCH.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "Common/StbCommon.h"

#include <assimp/Importer.hpp>
#include <assimp/Exporter.hpp>
#include <assimp/config.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace Assimp;

struct TempDirectory {
    TempDirectory() :
            path(std::filesystem::temp_directory_path() / "assimp_bta_unit_test") {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }

    ~TempDirectory() {
        std::filesystem::remove_all(path);
    }

    std::filesystem::path path;
};

struct Image {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;
};

bool LoadImage(const std::string &path, Image &image) {
    int components = 0;
    unsigned char *pixels = stbi_load(path.c_str(), &image.width, &image.height,
            &components, 4);
    if (pixels == nullptr || image.width <= 0 || image.height <= 0) {
        if (pixels != nullptr) {
            stbi_image_free(pixels);
        }
        return false;
    }
    image.rgba.assign(pixels,
            pixels + static_cast<std::size_t>(image.width * image.height * 4));
    stbi_image_free(pixels);
    return true;
}

std::array<unsigned char, 4> SampleImage(const Image &image, float u, float v) {
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);

    const float x = u * static_cast<float>(image.width) - 0.5f;
    const float y = v * static_cast<float>(image.height) - 0.5f;
    const int x0 = std::max(0, static_cast<int>(std::floor(x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(y)));
    const int x1 = std::min(x0 + 1, image.width - 1);
    const int y1 = std::min(y0 + 1, image.height - 1);
    const float fx = std::max(0.0f, x - static_cast<float>(x0));
    const float fy = std::max(0.0f, y - static_cast<float>(y0));

    std::array<unsigned char, 4> result{};
    for (unsigned int channel = 0; channel < 4; ++channel) {
        const auto fetch = [&](int sx, int sy) {
            return static_cast<float>(image.rgba[
                    (static_cast<std::size_t>(sy) * image.width + sx) * 4 + channel]);
        };
        const float top = fetch(x0, y0) * (1.0f - fx) + fetch(x1, y0) * fx;
        const float bottom = fetch(x0, y1) * (1.0f - fx) + fetch(x1, y1) * fx;
        result[channel] = static_cast<unsigned char>(
                std::clamp(top * (1.0f - fy) + bottom * fy + 0.5f, 0.0f, 255.0f));
    }
    return result;
}

bool ReadFileBytes(const std::string &path, std::vector<unsigned char> &bytes) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>());
    return true;
}

bool HasPngChunk(const std::vector<unsigned char> &bytes, const char *type) {
    for (std::size_t i = 0; i + 4 <= bytes.size(); ++i) {
        if (std::memcmp(bytes.data() + i, type, 4) == 0) {
            return true;
        }
    }
    return false;
}

std::string ResolveOriginalTexture(const std::string &reference,
        const std::string &root) {
    const std::size_t slash = reference.find_last_of("/\\");
    return root + (slash == std::string::npos ? reference
            : reference.substr(slash + 1));
}

aiTextureType BaseColorType(const aiMaterial *material) {
    return material->GetTextureCount(aiTextureType_BASE_COLOR) > 0
            ? aiTextureType_BASE_COLOR
            : aiTextureType_DIFFUSE;
}

void ExpectAtlasUvAndColors(const aiScene *scene, const std::string &atlasPath,
        const std::string &sourceRoot) {
    Image atlas;
    ASSERT_TRUE(LoadImage(atlasPath, atlas));

    std::unordered_map<std::string, Image> sourceImages;
    unsigned int sampledVertices = 0;
    for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex) {
        const aiMesh *mesh = scene->mMeshes[meshIndex];
        ASSERT_NE(nullptr, mesh);
        ASSERT_LT(mesh->mMaterialIndex, scene->mNumMaterials);
        const aiMaterial *material = scene->mMaterials[mesh->mMaterialIndex];

        aiString sourceReference;
        if (material->Get(AI_MATKEY_PP_BTA_SOURCE_TEXTURE, sourceReference) != AI_SUCCESS) {
            continue;
        }

        aiString texturePath;
        unsigned int uvIndex = 0;
        ASSERT_EQ(AI_SUCCESS, material->GetTexture(BaseColorType(material), 0,
                &texturePath, nullptr, &uvIndex));
        EXPECT_EQ(atlasPath, std::string(texturePath.C_Str()));
        ASSERT_LT(uvIndex, static_cast<unsigned int>(AI_MAX_NUMBER_OF_TEXTURECOORDS));
        ASSERT_NE(nullptr, mesh->mTextureCoords[uvIndex]);

        float rect[4] = {};
        unsigned int rectCount = 4;
        EXPECT_EQ(AI_SUCCESS, material->Get(AI_MATKEY_PP_BTA_UV_RECT,
                rect, &rectCount));
        EXPECT_EQ(4u, rectCount);

        const std::string sourcePath = ResolveOriginalTexture(
                sourceReference.C_Str(), sourceRoot);
        auto sourceIt = sourceImages.find(sourcePath);
        if (sourceIt == sourceImages.end()) {
            Image source;
            ASSERT_TRUE(LoadImage(sourcePath, source));
            sourceIt = sourceImages.emplace(sourcePath, std::move(source)).first;
        }
        const Image &source = sourceIt->second;

        for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex) {
            const aiFace &face = mesh->mFaces[faceIndex];
            for (unsigned int corner = 0; corner < face.mNumIndices; ++corner) {
                const aiVector3D &uv = mesh->mTextureCoords[uvIndex][face.mIndices[corner]];
                EXPECT_GE(uv.x, rect[0] - 1e-6f);
                EXPECT_LE(uv.x, rect[0] + rect[2] + 1e-6f);
                EXPECT_GE(uv.y, rect[1] - 1e-6f);
                EXPECT_LE(uv.y, rect[1] + rect[3] + 1e-6f);

                const float localU = (uv.x - rect[0]) / rect[2];
                const float localV = (uv.y - rect[1]) / rect[3];
                EXPECT_GE(localU, -1e-6f);
                EXPECT_LE(localU, 1.0f + 1e-6f);
                EXPECT_GE(localV, -1e-6f);
                EXPECT_LE(localV, 1.0f + 1e-6f);

                const auto atlasColor = SampleImage(atlas, uv.x, uv.y);
                const auto sourceColor = SampleImage(source, localU, localV);
                for (unsigned int channel = 0; channel < 4; ++channel) {
                    EXPECT_NEAR(static_cast<int>(sourceColor[channel]),
                            static_cast<int>(atlasColor[channel]), 2);
                }
                ++sampledVertices;
            }
        }
    }

    EXPECT_GT(sampledVertices, 0u);
}

} // namespace

TEST(utBakeTextureAtlas, disabledByDefault) {
    TempDirectory output;
    Importer importer;
    importer.SetPropertyString(AI_CONFIG_PP_BTA_OUTPUT_DIR, output.path.string());

    const aiScene *scene = importer.ReadFile(
            ASSIMP_TEST_MODELS_DIR "/OBJ/spider.obj", aiProcess_Triangulate);
    ASSERT_NE(nullptr, scene);
    ASSERT_FALSE(std::filesystem::exists(output.path / "assimp_basecolor_atlas.png"));

    const aiMaterial *material = scene->mMaterials[scene->mMeshes[0]->mMaterialIndex];
    aiString texturePath;
    ASSERT_EQ(AI_SUCCESS,
            material->GetTexture(BaseColorType(material), 0, &texturePath));
    EXPECT_NE(std::string("assimp_basecolor_atlas.png"), std::string(texturePath.C_Str()));
}

TEST(utBakeTextureAtlas, writesAtlasAndRemapsAllMaterials) {
    TempDirectory output;
    const std::string atlasName = "spider_atlas.png";
    Importer importer;
    importer.SetPropertyString(AI_CONFIG_PP_BTA_OUTPUT_DIR, output.path.string());
    importer.SetPropertyString(AI_CONFIG_PP_BTA_ATLAS_FILENAME, atlasName);
    importer.SetPropertyInteger(AI_CONFIG_PP_BTA_MAX_ATLAS_SIZE, 2048);

    const aiScene *scene = importer.ReadFile(
            ASSIMP_TEST_MODELS_DIR "/OBJ/spider.obj",
            aiProcess_BakeTextureAtlas | aiProcess_Triangulate);
    ASSERT_NE(nullptr, scene);

    const std::string atlasPath = (output.path / atlasName).string();
    ASSERT_TRUE(std::filesystem::exists(atlasPath));

    Image atlas;
    ASSERT_TRUE(LoadImage(atlasPath, atlas));
    EXPECT_LE(atlas.width, 2048);
    EXPECT_LE(atlas.height, 2048);

    std::vector<unsigned char> pngBytes;
    ASSERT_TRUE(ReadFileBytes(atlasPath, pngBytes));
    EXPECT_TRUE(HasPngChunk(pngBytes, "sRGB"));
    EXPECT_EQ(8, pngBytes[24]);
    EXPECT_EQ(6, pngBytes[25]);

    ExpectAtlasUvAndColors(scene, atlasPath,
            std::string(ASSIMP_TEST_MODELS_DIR) + "/OBJ/");
}

TEST(utBakeTextureAtlas, respectsMaximumAtlasSize) {
    TempDirectory output;
    Importer importer;
    importer.SetPropertyString(AI_CONFIG_PP_BTA_OUTPUT_DIR, output.path.string());
    importer.SetPropertyInteger(AI_CONFIG_PP_BTA_MAX_ATLAS_SIZE, 512);

    const aiScene *scene = importer.ReadFile(
            ASSIMP_TEST_MODELS_DIR "/OBJ/spider.obj",
            aiProcess_BakeTextureAtlas | aiProcess_Triangulate);
    ASSERT_NE(nullptr, scene);

    Image atlas;
    ASSERT_TRUE(LoadImage((output.path / "assimp_basecolor_atlas.png").string(), atlas));
    EXPECT_LE(atlas.width, 512);
    EXPECT_LE(atlas.height, 512);
}

TEST(utBakeTextureAtlas, preservesAlphaFromPngBaseColor) {
    TempDirectory output;
    const std::string atlasName = "alpha_atlas.png";
    Importer importer;
    importer.SetPropertyString(AI_CONFIG_PP_BTA_OUTPUT_DIR, output.path.string());
    importer.SetPropertyString(AI_CONFIG_PP_BTA_ATLAS_FILENAME, atlasName);

    const aiScene *scene = importer.ReadFile(
            ASSIMP_TEST_MODELS_DIR "/glTF2/ClearCoat-glTF/ClearCoatTest.gltf",
            aiProcess_BakeTextureAtlas | aiProcess_Triangulate);
    ASSERT_NE(nullptr, scene);

    Image source;
    ASSERT_TRUE(LoadImage(
            ASSIMP_TEST_MODELS_DIR "/glTF2/ClearCoat-glTF/PartialCoating_Alpha.png",
            source));
    bool hasTransparentPixel = false;
    for (std::size_t i = 3; i < source.rgba.size(); i += 4) {
        if (source.rgba[i] != 255) {
            hasTransparentPixel = true;
            break;
        }
    }
    ASSERT_TRUE(hasTransparentPixel);

    ExpectAtlasUvAndColors(scene, (output.path / atlasName).string(),
            std::string(ASSIMP_TEST_MODELS_DIR) + "/glTF2/ClearCoat-glTF/");
}

TEST(utBakeTextureAtlas, handlesOutOfRangeUvAndSharedVertices) {
    TempDirectory output;
    const std::filesystem::path modelPath = output.path / "wrapped.obj";
    const std::filesystem::path materialPath = output.path / "wrapped.mtl";
    const std::filesystem::path texturePath = output.path / "texture.png";
    std::filesystem::copy_file(
            std::string(ASSIMP_TEST_MODELS_DIR) +
                    "/glTF2/ClearCoat-glTF/PartialCoating_Alpha.png",
            texturePath);

    {
        std::ofstream material(materialPath);
        material << "newmtl mat\nKd 1 1 1\nmap_Kd texture.png\n"
                    "map_Bump texture.png\n";
    }
    {
        std::ofstream model(modelPath);
        model << "mtllib wrapped.mtl\n"
                 "v 0 0 0\n"
                 "v 1 0 0\n"
                 "v 1 1 0\n"
                 "v 0 1 0\n"
                 "vt -0.25 0.25\n"
                 "vt 0.75 0.25\n"
                 "vt 0.75 1.75\n"
                 "vt 1.25 0.75\n"
                 "usemtl mat\n"
                 "f 1/1 2/2 3/3\n"
                 "f 1/1 3/3 4/4\n";
    }

    Importer importer;
    importer.SetPropertyString(AI_CONFIG_PP_BTA_OUTPUT_DIR, output.path.string());
    importer.SetPropertyInteger(AI_CONFIG_PP_BTA_MAX_ATLAS_SIZE, 512);
    const aiScene *scene = importer.ReadFile(modelPath.string(),
            aiProcess_BakeTextureAtlas | aiProcess_Triangulate);
    ASSERT_NE(nullptr, scene);

    const aiMaterial *material =
            scene->mMaterials[scene->mMeshes[0]->mMaterialIndex];
    unsigned int baseUvIndex = 0;
    aiString mappedTexturePath;
    ASSERT_EQ(AI_SUCCESS, material->GetTexture(aiTextureType_DIFFUSE, 0,
            &mappedTexturePath, nullptr, &baseUvIndex));
    unsigned int bumpUvIndex = 0;
    ASSERT_EQ(AI_SUCCESS, material->GetTexture(aiTextureType_HEIGHT, 0,
            &mappedTexturePath, nullptr, &bumpUvIndex));
    EXPECT_NE(baseUvIndex, bumpUvIndex);
    EXPECT_EQ(0u, bumpUvIndex);

    bool sourceUvPreserved = false;
    const aiMesh *mesh = scene->mMeshes[0];
    for (unsigned int vertex = 0; vertex < mesh->mNumVertices; ++vertex) {
        const aiVector3D &uv = mesh->mTextureCoords[0][vertex];
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) {
            sourceUvPreserved = true;
            break;
        }
    }
    EXPECT_TRUE(sourceUvPreserved);

    ExpectAtlasUvAndColors(scene,
            (output.path / "assimp_basecolor_atlas.png").string(),
            output.path.string() + "/");
}

TEST(utBakeTextureAtlas, exportedObjKeepsAtlasCoordinates) {
    TempDirectory output;
    Importer importer;
    importer.SetPropertyString(AI_CONFIG_PP_BTA_OUTPUT_DIR, output.path.string());
    const aiScene *scene = importer.ReadFile(
            ASSIMP_TEST_MODELS_DIR "/OBJ/spider.obj",
            aiProcess_BakeTextureAtlas | aiProcess_Triangulate);
    ASSERT_NE(nullptr, scene);

    const std::filesystem::path exportedPath = output.path / "exported.obj";
    Exporter exporter;
    ASSERT_EQ(AI_SUCCESS,
            exporter.Export(scene, "obj", exportedPath.string()));
    ASSERT_TRUE(std::filesystem::exists(exportedPath));

    Importer reimport;
    const aiScene *exported = reimport.ReadFile(exportedPath.string(),
            aiProcess_Triangulate);
    ASSERT_NE(nullptr, exported);

    unsigned int checkedUvs = 0;
    for (unsigned int meshIndex = 0; meshIndex < exported->mNumMeshes; ++meshIndex) {
        const aiMesh *mesh = exported->mMeshes[meshIndex];
        for (unsigned int channel = 0; channel < AI_MAX_NUMBER_OF_TEXTURECOORDS;
                ++channel) {
            if (mesh->mTextureCoords[channel] == nullptr) {
                continue;
            }
            for (unsigned int vertex = 0; vertex < mesh->mNumVertices; ++vertex) {
                const aiVector3D &uv = mesh->mTextureCoords[channel][vertex];
                EXPECT_GE(uv.x, 0.0f);
                EXPECT_LE(uv.x, 1.0f);
                EXPECT_GE(uv.y, 0.0f);
                EXPECT_LE(uv.y, 1.0f);
                ++checkedUvs;
            }
        }
    }
    EXPECT_GT(checkedUvs, 0u);
}
