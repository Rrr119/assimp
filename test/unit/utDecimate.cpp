/*
---------------------------------------------------------------------------
Open Asset Import Library (assimp)
---------------------------------------------------------------------------

Copyright (c) 2006-2026, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the
following conditions are met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.

* Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the
  following disclaimer in the documentation and/or other
  materials provided with the distribution.

* Neither the name of the assimp team, nor the names of its
  contributors may be used to endorse or promote products
  derived from this software without specific prior
  written permission of the assimp team.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
---------------------------------------------------------------------------
*/

#include "UnitTestPCH.h"

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

using namespace Assimp;

namespace {

struct SceneStats {
    std::size_t triangles = 0;
    aiVector3D min = aiVector3D(std::numeric_limits<ai_real>::max(),
            std::numeric_limits<ai_real>::max(),
            std::numeric_limits<ai_real>::max());
    aiVector3D max = aiVector3D(-std::numeric_limits<ai_real>::max(),
            -std::numeric_limits<ai_real>::max(),
            -std::numeric_limits<ai_real>::max());
};

SceneStats GetSceneStats(const aiScene *scene) {
    SceneStats stats;
    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh *mesh = scene->mMeshes[i];
        for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
            if (mesh->mFaces[f].mNumIndices == 3) {
                ++stats.triangles;
            }
        }
        for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            stats.min = std::min(stats.min, mesh->mVertices[v]);
            stats.max = std::max(stats.max, mesh->mVertices[v]);
        }
    }
    return stats;
}

std::string ModelPath(const char *relativePath) {
    return std::string(ASSIMP_TEST_MODELS_DIR) + "/" + relativePath;
}

} // namespace

TEST(utDecimate, HighPolyTargetBoundsAndMaterials) {
    const std::string path = ModelPath("glTF2/2CylinderEngine-glTF-Binary/2CylinderEngine.glb");

    Importer baselineImporter;
    const aiScene *baseline = baselineImporter.ReadFile(
            path, static_cast<aiPostProcessFlags>(aiProcess_Triangulate));
    ASSERT_NE(nullptr, baseline) << baselineImporter.GetErrorString();
    const SceneStats baselineStats = GetSceneStats(baseline);
    ASSERT_GT(baselineStats.triangles, 70000u);

    const unsigned int targetFaces = 30000;
    Importer decimatedImporter;
    decimatedImporter.SetPropertyInteger(AI_CONFIG_PP_DECIMATE_TARGET_FACES, targetFaces);
    const aiPostProcessFlags decimateFlags =
            aiProcess_Triangulate | aiProcess_Decimate | aiProcess_ValidateDataStructure;
    EXPECT_TRUE(decimatedImporter.ValidateFlags(decimateFlags));
    const aiScene *decimated = decimatedImporter.ReadFile(path, decimateFlags);
    ASSERT_NE(nullptr, decimated) << decimatedImporter.GetErrorString();
    const SceneStats decimatedStats = GetSceneStats(decimated);

    EXPECT_LE(decimatedStats.triangles, targetFaces);
    EXPECT_GE(decimatedStats.triangles, targetFaces - 1000);
    EXPECT_EQ(baseline->mNumMeshes, decimated->mNumMeshes);
    EXPECT_EQ(baseline->mNumMaterials, decimated->mNumMaterials);
    for (unsigned int i = 0; i < baseline->mNumMeshes; ++i) {
        EXPECT_EQ(baseline->mMeshes[i]->mMaterialIndex,
                decimated->mMeshes[i]->mMaterialIndex);
    }

    const ai_real diagonal = (baselineStats.max - baselineStats.min).Length();
    const ai_real tolerance = diagonal * ai_real(0.03);
    for (unsigned int axis = 0; axis < 3; ++axis) {
        EXPECT_LE(std::abs(decimatedStats.min[axis] - baselineStats.min[axis]), tolerance);
        EXPECT_LE(std::abs(decimatedStats.max[axis] - baselineStats.max[axis]), tolerance);
    }
}

TEST(utDecimate, RatioUvSeamsStayInBounds) {
    const std::string path = ModelPath("Collada/duck_triangulate.dae");

    Importer baselineImporter;
    const aiScene *baseline = baselineImporter.ReadFile(
            path, static_cast<aiPostProcessFlags>(aiProcess_Triangulate));
    ASSERT_NE(nullptr, baseline) << baselineImporter.GetErrorString();
    const SceneStats baselineStats = GetSceneStats(baseline);
    ASSERT_GT(baselineStats.triangles, 4000u);
    ASSERT_TRUE(baseline->mMeshes[0]->HasTextureCoords(0));

    aiVector3D uvMin = baseline->mMeshes[0]->mTextureCoords[0][0];
    aiVector3D uvMax = uvMin;
    for (unsigned int v = 0; v < baseline->mMeshes[0]->mNumVertices; ++v) {
        const aiVector3D &uv = baseline->mMeshes[0]->mTextureCoords[0][v];
        for (unsigned int component = 0; component < 3; ++component) {
            uvMin[component] = std::min(uvMin[component], uv[component]);
            uvMax[component] = std::max(uvMax[component], uv[component]);
        }
    }

    const ai_real ratio = ai_real(0.25);
    Importer decimatedImporter;
    decimatedImporter.SetPropertyFloat(AI_CONFIG_PP_DECIMATE_TARGET_RATIO, ratio);
    const aiScene *decimated = decimatedImporter.ReadFile(path,
            static_cast<aiPostProcessFlags>(aiProcess_Triangulate | aiProcess_Decimate));
    ASSERT_NE(nullptr, decimated) << decimatedImporter.GetErrorString();
    const SceneStats decimatedStats = GetSceneStats(decimated);

    const std::size_t expectedTarget = static_cast<std::size_t>(
            std::floor(double(baselineStats.triangles) * double(ratio)));
    EXPECT_LE(decimatedStats.triangles, expectedTarget);
    EXPECT_GE(decimatedStats.triangles, expectedTarget - 100);
    ASSERT_TRUE(decimated->mMeshes[0]->HasTextureCoords(0));

    const ai_real uvEpsilon = ai_real(1e-4);
    for (unsigned int v = 0; v < decimated->mMeshes[0]->mNumVertices; ++v) {
        const aiVector3D &uv = decimated->mMeshes[0]->mTextureCoords[0][v];
        for (unsigned int component = 0; component < 3; ++component) {
            EXPECT_GE(uv[component], uvMin[component] - uvEpsilon);
            EXPECT_LE(uv[component], uvMax[component] + uvEpsilon);
        }
    }
}

TEST(utDecimate, SkinnedWeightsRemainValid) {
    const std::string path = ModelPath("IRR/assets/dwarf.x");

    Importer baselineImporter;
    const aiScene *baseline = baselineImporter.ReadFile(
            path, static_cast<aiPostProcessFlags>(aiProcess_Triangulate));
    ASSERT_NE(nullptr, baseline) << baselineImporter.GetErrorString();

    Importer decimatedImporter;
    decimatedImporter.SetPropertyInteger(AI_CONFIG_PP_DECIMATE_TARGET_FACES, 1000);
    const aiScene *decimated = decimatedImporter.ReadFile(path,
            static_cast<aiPostProcessFlags>(
                    aiProcess_Triangulate | aiProcess_Decimate | aiProcess_ValidateDataStructure));
    ASSERT_NE(nullptr, decimated) << decimatedImporter.GetErrorString();

    bool foundSkin = false;
    for (unsigned int i = 0; i < decimated->mNumMeshes; ++i) {
        const aiMesh *mesh = decimated->mMeshes[i];
        if (!mesh->HasBones()) {
            continue;
        }
        foundSkin = true;
        for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            ai_real total = 0.0;
            for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
                const aiBone *bone = mesh->mBones[b];
                for (unsigned int w = 0; w < bone->mNumWeights; ++w) {
                    if (bone->mWeights[w].mVertexId == v) {
                        EXPECT_LT(bone->mWeights[w].mVertexId, mesh->mNumVertices);
                        total += bone->mWeights[w].mWeight;
                    }
                }
            }
            EXPECT_NEAR(ai_real(1.0), total, ai_real(1e-3));
        }
    }
    EXPECT_TRUE(foundSkin);
}

TEST(utDecimate, DisabledByDefault) {
    const std::string path = ModelPath("Collada/duck_triangulate.dae");
    Importer importer;
    const aiScene *scene = importer.ReadFile(
            path, static_cast<aiPostProcessFlags>(aiProcess_Triangulate));
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    EXPECT_EQ(4212u, GetSceneStats(scene).triangles);
}
