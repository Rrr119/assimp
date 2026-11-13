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

/** @file  DecimateProcess.cpp
 *  @brief QEM mesh decimation post-processing step.
 */

#ifndef ASSIMP_BUILD_NO_DECIMATE_PROCESS

#include "DecimateProcess.h"
#ifndef ASSIMP_BUILD_NO_JOINVERTICES_PROCESS
#include "JoinVerticesProcess.h"
#endif

#include <assimp/Exceptional.h>
#include <assimp/DefaultLogger.hpp>
#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/mesh.h>
#include <assimp/scene.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Assimp {

namespace {

constexpr ai_real kDefaultTargetRatio = AI_DECIMATE_DEFAULT_TARGET_RATIO;
constexpr ai_real kDefaultNormalWeight = AI_DECIMATE_DEFAULT_NORMAL_ANGLE_WEIGHT;
constexpr ai_real kDefaultUvSeamEpsilon = AI_DECIMATE_DEFAULT_UV_SEAM_EPSILON;
constexpr ai_real kUvEqualityEpsilon = ai_real(1e-6);

struct Quadric {
    double a00 = 0.0;
    double a01 = 0.0;
    double a02 = 0.0;
    double a03 = 0.0;
    double a11 = 0.0;
    double a12 = 0.0;
    double a13 = 0.0;
    double a22 = 0.0;
    double a23 = 0.0;
    double a33 = 0.0;

    void AddPlane(const aiVector3D &normal, double d, double weight) {
        a00 += weight * normal.x * normal.x;
        a01 += weight * normal.x * normal.y;
        a02 += weight * normal.x * normal.z;
        a03 += weight * normal.x * d;
        a11 += weight * normal.y * normal.y;
        a12 += weight * normal.y * normal.z;
        a13 += weight * normal.y * d;
        a22 += weight * normal.z * normal.z;
        a23 += weight * normal.z * d;
        a33 += weight * d * d;
    }

    void Add(const Quadric &other) {
        a00 += other.a00;
        a01 += other.a01;
        a02 += other.a02;
        a03 += other.a03;
        a11 += other.a11;
        a12 += other.a12;
        a13 += other.a13;
        a22 += other.a22;
        a23 += other.a23;
        a33 += other.a33;
    }

    double Evaluate(const aiVector3D &v) const {
        const double x = v.x;
        const double y = v.y;
        const double z = v.z;
        return a00 * x * x + 2.0 * a01 * x * y + 2.0 * a02 * x * z +
               2.0 * a03 * x + a11 * y * y + 2.0 * a12 * y * z +
               2.0 * a13 * y + a22 * z * z + 2.0 * a23 * z + a33;
    }
};

struct VertexWeight {
    unsigned int bone = 0;
    double weight = 0.0;
};

struct VertexData {
    aiVector3D position;
    aiVector3D normal;
    aiVector3D tangent;
    aiVector3D bitangent;
    std::vector<aiVector3D> texcoords;
    std::vector<aiColor4D> colors;
    std::vector<VertexWeight> weights;
    std::vector<unsigned int> faces;
    Quadric quadric;
    std::uint64_t version = 0;
    bool alive = true;
    bool locked = false;
};

struct FaceData {
    std::array<unsigned int, 3> vertices = { 0u, 0u, 0u };
    aiVector3D normal;
    bool alive = true;
};

struct CollapseCandidate {
    double cost = 0.0;
    unsigned int a = 0u;
    unsigned int b = 0u;
    std::uint64_t versionA = 0u;
    std::uint64_t versionB = 0u;

    template <typename VertexContainer>
    bool IsCurrent(const VertexContainer &vertices) const {
        return a < vertices.size() && b < vertices.size() &&
                vertices[a].alive && vertices[b].alive &&
                vertices[a].version == versionA && vertices[b].version == versionB;
    }
};

struct CandidateCompare {
    bool operator()(const CollapseCandidate &lhs, const CollapseCandidate &rhs) const {
        return lhs.cost > rhs.cost;
    }
};

inline aiVector3D Lerp(const aiVector3D &a, const aiVector3D &b, ai_real t) {
    return a * (ai_real(1.0) - t) + b * t;
}

inline aiColor4D Lerp(const aiColor4D &a, const aiColor4D &b, ai_real t) {
    return aiColor4D(
            a.r * (ai_real(1.0) - t) + b.r * t,
            a.g * (ai_real(1.0) - t) + b.g * t,
            a.b * (ai_real(1.0) - t) + b.b * t,
            a.a * (ai_real(1.0) - t) + b.a * t);
}

inline bool IsValidVector(const aiVector3D &v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

inline bool NormalizeIfPossible(aiVector3D &v) {
    const ai_real lengthSquared = v.SquareLength();
    if (lengthSquared <= ai_epsilon) {
        return false;
    }
    v /= std::sqrt(lengthSquared);
    return true;
}

inline bool FaceContains(const FaceData &face, unsigned int vertex) {
    return face.vertices[0] == vertex || face.vertices[1] == vertex || face.vertices[2] == vertex;
}

class MeshDecimator {
public:
    MeshDecimator(aiMesh *mesh, const aiDecimateSettings &settings) :
            mMesh(mesh),
            mSettings(settings),
            mVertexCount(mesh->mNumVertices),
            mFaceCount(mesh->mNumFaces),
            mDiagonal(1.0),
            mHasNormals(mesh->HasNormals()),
            mHasTangents(mesh->HasTangentsAndBitangents()),
            mUvChannelCount(0),
            mColorChannelCount(0),
            mBoneCount(mesh->mNumBones) {
        for (unsigned int channel = 0; channel < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++channel) {
            if (!mesh->HasTextureCoords(channel)) {
                break;
            }
            ++mUvChannelCount;
        }
        for (unsigned int channel = 0; channel < AI_MAX_NUMBER_OF_COLOR_SETS; ++channel) {
            if (!mesh->HasVertexColors(channel)) {
                break;
            }
            ++mColorChannelCount;
        }
    }

    bool CanDecimate() const {
        if (mMesh == nullptr || mMesh->mNumAnimMeshes != 0 ||
                mVertexCount < 4 || mFaceCount < 4) {
            return false;
        }
        for (unsigned int i = 0; i < mFaceCount; ++i) {
            if (mMesh->mFaces[i].mNumIndices != 3) {
                return false;
            }
        }
        return true;
    }

    unsigned int Simplify(unsigned int targetFaceCount) {
        if (!CanDecimate() || targetFaceCount >= mFaceCount) {
            return mFaceCount;
        }

        Initialize();
        BuildQuadrics();
        MarkUvSeams();
        AddBoundaryConstraints();
        BuildEdges();

        while (mFaceCount > targetFaceCount && !mQueue.empty()) {
            const CollapseCandidate candidate = mQueue.top();
            mQueue.pop();

            if (!candidate.IsCurrent(mVertices)) {
                continue;
            }

            ai_real t = ai_real(0.5);
            std::vector<unsigned int> edgeFaces;
            if (!EvaluateCandidate(candidate.a, candidate.b, t, edgeFaces)) {
                continue;
            }
            Collapse(candidate.a, candidate.b, t, edgeFaces);
        }

        if (mFaceCount < mMesh->mNumFaces) {
            RebuildMesh();
        }
        return mFaceCount;
    }

private:
    void Initialize() {
        mVertices.resize(mVertexCount);
        mFaces.resize(mFaceCount);

        aiVector3D min, max;
        if (mVertexCount != 0) {
            min = max = mMesh->mVertices[0];
        }
        for (unsigned int i = 0; i < mVertexCount; ++i) {
            VertexData &vertex = mVertices[i];
            vertex.position = mMesh->mVertices[i];
            min = std::min(min, vertex.position);
            max = std::max(max, vertex.position);

            if (mHasNormals) {
                vertex.normal = mMesh->mNormals[i];
            }
            if (mHasTangents) {
                vertex.tangent = mMesh->mTangents[i];
                vertex.bitangent = mMesh->mBitangents[i];
            }
            vertex.texcoords.resize(mUvChannelCount);
            for (unsigned int channel = 0; channel < mUvChannelCount; ++channel) {
                vertex.texcoords[channel] = mMesh->mTextureCoords[channel][i];
            }
            vertex.colors.resize(mColorChannelCount);
            for (unsigned int channel = 0; channel < mColorChannelCount; ++channel) {
                vertex.colors[channel] = mMesh->mColors[channel][i];
            }
        }

        mDiagonal = mVertexCount > 1 ? (max - min).Length() : ai_real(1.0);
        if (mDiagonal <= ai_epsilon) {
            mDiagonal = ai_real(1.0);
        }
        mAreaEpsilonSquared = std::max(double(1e-20), double(mDiagonal) * double(mDiagonal) * 1e-16);

        for (unsigned int i = 0; i < mFaceCount; ++i) {
            const aiFace &source = mMesh->mFaces[i];
            FaceData &face = mFaces[i];
            face.vertices[0] = source.mIndices[0];
            face.vertices[1] = source.mIndices[1];
            face.vertices[2] = source.mIndices[2];
            for (unsigned int vertex : face.vertices) {
                mVertices[vertex].faces.push_back(i);
            }
            UpdateFaceNormal(i);
        }

        for (unsigned int i = 0; i < mBoneCount; ++i) {
            const aiBone *bone = mMesh->mBones[i];
            for (unsigned int w = 0; w < bone->mNumWeights; ++w) {
                const aiVertexWeight &weight = bone->mWeights[w];
                if (weight.mVertexId < mVertexCount) {
                    mVertices[weight.mVertexId].weights.push_back({ i, double(weight.mWeight) });
                }
            }
        }
    }

    void UpdateFaceNormal(unsigned int faceIndex) {
        FaceData &face = mFaces[faceIndex];
        const aiVector3D &a = mVertices[face.vertices[0]].position;
        const aiVector3D &b = mVertices[face.vertices[1]].position;
        const aiVector3D &c = mVertices[face.vertices[2]].position;
        face.normal = (b - a) ^ (c - a);
        NormalizeIfPossible(face.normal);
    }

    void BuildQuadrics() {
        for (unsigned int i = 0; i < mFaceCount; ++i) {
            FaceData &face = mFaces[i];
            const aiVector3D &a = mVertices[face.vertices[0]].position;
            const aiVector3D &b = mVertices[face.vertices[1]].position;
            const aiVector3D &c = mVertices[face.vertices[2]].position;
            const aiVector3D cross = (b - a) ^ (c - a);
            const ai_real crossLength = cross.Length();
            if (crossLength <= ai_epsilon) {
                continue;
            }

            const aiVector3D normal = cross / crossLength;
            const double area = double(crossLength) * 0.5;
            const double d = -double(normal * a);
            Quadric quadric;
            quadric.AddPlane(normal, d, area);
            for (unsigned int vertex : face.vertices) {
                mVertices[vertex].quadric.Add(quadric);
            }
        }
    }

    void MarkUvSeams() {
        if (mUvChannelCount == 0 || mVertexCount < 2) {
            return;
        }

        using Cell = std::array<std::int64_t, 3>;
        const ai_real tolerance = std::max(ai_real(1e-8), mDiagonal * mSettings.mUvSeamEpsilon);
        std::map<Cell, std::vector<unsigned int>> groups;

        for (unsigned int i = 0; i < mVertexCount; ++i) {
            const aiVector3D &position = mVertices[i].position;
            Cell cell = {
                static_cast<std::int64_t>(std::llround(position.x / tolerance)),
                static_cast<std::int64_t>(std::llround(position.y / tolerance)),
                static_cast<std::int64_t>(std::llround(position.z / tolerance))
            };
            groups[cell].push_back(i);
        }

        for (const auto &entry : groups) {
            const std::vector<unsigned int> &group = entry.second;
            if (group.size() < 2) {
                continue;
            }

            for (unsigned int channel = 0; channel < mUvChannelCount; ++channel) {
                const aiVector3D &reference = mVertices[group.front()].texcoords[channel];
                bool differs = false;
                for (unsigned int i = 1; i < group.size() && !differs; ++i) {
                    const aiVector3D &uv = mVertices[group[i]].texcoords[channel];
                    differs = std::abs(reference.x - uv.x) > kUvEqualityEpsilon ||
                            std::abs(reference.y - uv.y) > kUvEqualityEpsilon ||
                            std::abs(reference.z - uv.z) > kUvEqualityEpsilon;
                }
                if (differs) {
                    for (unsigned int vertex : group) {
                        mVertices[vertex].locked = true;
                    }
                    break;
                }
            }
        }
    }

    void BuildEdges() {
        for (unsigned int i = 0; i < mFaceCount; ++i) {
            const FaceData &face = mFaces[i];
            PushEdge(face.vertices[0], face.vertices[1]);
            PushEdge(face.vertices[1], face.vertices[2]);
            PushEdge(face.vertices[2], face.vertices[0]);
        }
    }

    void AddBoundaryConstraints() {
        using EdgeKey = std::pair<unsigned int, unsigned int>;
        std::map<EdgeKey, unsigned int> edgeCount;
        for (unsigned int i = 0; i < mFaceCount; ++i) {
            const FaceData &face = mFaces[i];
            for (unsigned int e = 0; e < 3; ++e) {
                unsigned int a = face.vertices[e];
                unsigned int b = face.vertices[(e + 1) % 3];
                if (a > b) {
                    std::swap(a, b);
                }
                ++edgeCount[{ a, b }];
            }
        }

        for (unsigned int i = 0; i < mFaceCount; ++i) {
            const FaceData &face = mFaces[i];
            for (unsigned int e = 0; e < 3; ++e) {
                unsigned int a = face.vertices[e];
                unsigned int b = face.vertices[(e + 1) % 3];
                const EdgeKey key = a < b ? EdgeKey{ a, b } : EdgeKey{ b, a };
                if (edgeCount[key] != 1) {
                    continue;
                }
                const aiVector3D edge = mVertices[b].position - mVertices[a].position;
                const ai_real edgeLength = edge.Length();
                if (edgeLength <= ai_epsilon) {
                    continue;
                }
                aiVector3D boundaryNormal = (edge / edgeLength) ^ face.normal;
                if (!NormalizeIfPossible(boundaryNormal)) {
                    continue;
                }
                const ai_real weight = edgeLength * edgeLength;
                Quadric constraint;
                constraint.AddPlane(boundaryNormal,
                        -double(boundaryNormal * mVertices[a].position),
                        double(weight));
                mVertices[a].quadric.Add(constraint);
                mVertices[b].quadric.Add(constraint);
            }
        }
    }

    void PushEdge(unsigned int a, unsigned int b) {
        if (a == b || !mVertices[a].alive || !mVertices[b].alive) {
            return;
        }
        const std::pair<unsigned int, unsigned int> key = a < b
                ? std::make_pair(a, b)
                : std::make_pair(b, a);
        auto it = mQueuedEdges.find(key);
        if (it != mQueuedEdges.end() &&
                it->second.first == mVertices[a].version &&
                it->second.second == mVertices[b].version) {
            return;
        }
        mQueuedEdges[key] = std::make_pair(mVertices[a].version, mVertices[b].version);

        ai_real t = ai_real(0.5);
        std::vector<unsigned int> edgeFaces;
        double cost = 0.0;
        if (EvaluateCandidate(a, b, t, edgeFaces, &cost)) {
            mQueue.push({ cost, a, b, mVertices[a].version, mVertices[b].version });
        }
    }

    bool EvaluateCandidate(unsigned int a, unsigned int b, ai_real &t,
            std::vector<unsigned int> &edgeFaces, double *outCost = nullptr) const {
        if (a == b || !mVertices[a].alive || !mVertices[b].alive ||
                mVertices[a].locked || mVertices[b].locked) {
            return false;
        }

        edgeFaces.clear();
        for (unsigned int faceIndex : mVertices[a].faces) {
            const FaceData &face = mFaces[faceIndex];
            if (face.alive && FaceContains(face, b)) {
                edgeFaces.push_back(faceIndex);
                if (edgeFaces.size() > 2) {
                    return false;
                }
            }
        }
        if (edgeFaces.empty()) {
            return false;
        }

        std::vector<unsigned int> neighborsA;
        std::vector<unsigned int> neighborsB;
        GatherNeighbors(a, b, neighborsA);
        GatherNeighbors(b, a, neighborsB);
        std::sort(neighborsA.begin(), neighborsA.end());
        neighborsA.erase(std::unique(neighborsA.begin(), neighborsA.end()), neighborsA.end());
        std::sort(neighborsB.begin(), neighborsB.end());
        neighborsB.erase(std::unique(neighborsB.begin(), neighborsB.end()), neighborsB.end());

        std::vector<unsigned int> common;
        std::set_intersection(neighborsA.begin(), neighborsA.end(), neighborsB.begin(), neighborsB.end(),
                std::back_inserter(common));
        if (common.size() != edgeFaces.size()) {
            return false;
        }

        const Quadric combined = [&]() {
            Quadric quadric = mVertices[a].quadric;
            quadric.Add(mVertices[b].quadric);
            return quadric;
        }();
        const aiVector3D optimal = ComputeOptimalPosition(combined, mVertices[a].position, mVertices[b].position);
        const aiVector3D edge = mVertices[b].position - mVertices[a].position;
        const double edgeLengthSquared = double(edge.SquareLength());
        if (edgeLengthSquared <= 1e-20) {
            return false;
        }

        const ai_real optimalT = ComputeSegmentParameter(optimal, mVertices[a].position, mVertices[b].position);
        const std::array<ai_real, 5> candidates = {
            optimalT,
            ai_real(0.0),
            ai_real(1.0),
            ai_real(0.5),
            ai_real(0.25)
        };

        double bestCost = std::numeric_limits<double>::infinity();
        ai_real bestT = ai_real(0.5);
        for (ai_real candidateT : candidates) {
            const aiVector3D position = Lerp(mVertices[a].position, mVertices[b].position, candidateT);
            double faceNormalPenalty = 0.0;
            if (!EvaluateFaceNormals(a, b, edgeFaces, position, edgeLengthSquared, faceNormalPenalty)) {
                continue;
            }

            double cost = std::max(0.0, combined.Evaluate(position));
            cost += NormalAnglePenalty(a, b, edgeLengthSquared);
            cost += faceNormalPenalty;
            if (edgeFaces.size() == 1) {
                cost += edgeLengthSquared * 0.5;
            }

            if (cost < bestCost) {
                bestCost = cost;
                bestT = candidateT;
            }
        }

        if (!std::isfinite(bestCost)) {
            return false;
        }
        t = bestT;
        if (outCost != nullptr) {
            *outCost = bestCost;
        }
        return true;
    }

    void GatherNeighbors(unsigned int vertex, unsigned int excluded, std::vector<unsigned int> &out) const {
        for (unsigned int faceIndex : mVertices[vertex].faces) {
            const FaceData &face = mFaces[faceIndex];
            if (!face.alive) {
                continue;
            }
            for (unsigned int candidate : face.vertices) {
                if (candidate != vertex && candidate != excluded) {
                    out.push_back(candidate);
                }
            }
        }
    }

    static aiVector3D ComputeOptimalPosition(const Quadric &q, const aiVector3D &a, const aiVector3D &b) {
        const double matrix[3][3] = {
            { q.a00, q.a01, q.a02 },
            { q.a01, q.a11, q.a12 },
            { q.a02, q.a12, q.a22 }
        };
        const double rhs[3] = { -q.a03, -q.a13, -q.a23 };
        const double determinant =
                matrix[0][0] * (matrix[1][1] * matrix[2][2] - matrix[1][2] * matrix[2][1]) -
                matrix[0][1] * (matrix[1][0] * matrix[2][2] - matrix[1][2] * matrix[2][0]) +
                matrix[0][2] * (matrix[1][0] * matrix[2][1] - matrix[1][1] * matrix[2][0]);

        const double scale = std::abs(matrix[0][0]) + std::abs(matrix[1][1]) + std::abs(matrix[2][2]) + 1.0;
        if (std::abs(determinant) <= 1e-12 * scale * scale * scale) {
            return Lerp(a, b, ai_real(0.5));
        }

        const auto Det3 = [](double m00, double m01, double m02,
                                double m10, double m11, double m12,
                                double m20, double m21, double m22) {
            return m00 * (m11 * m22 - m12 * m21) -
                    m01 * (m10 * m22 - m12 * m20) +
                    m02 * (m10 * m21 - m11 * m20);
        };

        const double x = Det3(rhs[0], matrix[0][1], matrix[0][2],
                              rhs[1], matrix[1][1], matrix[1][2],
                              rhs[2], matrix[2][1], matrix[2][2]) /
                determinant;
        const double y = Det3(matrix[0][0], rhs[0], matrix[0][2],
                              matrix[1][0], rhs[1], matrix[1][2],
                              matrix[2][0], rhs[2], matrix[2][2]) /
                determinant;
        const double z = Det3(matrix[0][0], matrix[0][1], rhs[0],
                              matrix[1][0], matrix[1][1], rhs[1],
                              matrix[2][0], matrix[2][1], rhs[2]) /
                determinant;
        return aiVector3D(static_cast<ai_real>(x), static_cast<ai_real>(y), static_cast<ai_real>(z));
    }

    static ai_real ComputeSegmentParameter(const aiVector3D &point, const aiVector3D &a, const aiVector3D &b) {
        const aiVector3D edge = b - a;
        const ai_real lengthSquared = edge.SquareLength();
        if (lengthSquared <= ai_epsilon) {
            return ai_real(0.5);
        }
        const ai_real t = ((point - a) * edge) / lengthSquared;
        return std::max(ai_real(0.0), std::min(ai_real(1.0), t));
    }

    double NormalAnglePenalty(unsigned int a, unsigned int b, double edgeLengthSquared) const {
        if (!mHasNormals) {
            return 0.0;
        }
        aiVector3D normalA = mVertices[a].normal;
        aiVector3D normalB = mVertices[b].normal;
        if (!NormalizeIfPossible(normalA) || !NormalizeIfPossible(normalB)) {
            return 0.0;
        }
        const ai_real dot = std::max(ai_real(-1.0), std::min(ai_real(1.0), normalA * normalB));
        const ai_real angle = ai_real(1.0) - dot;
        return double(mSettings.mNormalAngleWeight) * double(angle) * double(angle) * edgeLengthSquared;
    }

    bool EvaluateFaceNormals(unsigned int a, unsigned int b, const std::vector<unsigned int> &edgeFaces,
            const aiVector3D &position, double edgeLengthSquared, double &penalty) const {
        penalty = 0.0;
        for (unsigned int faceIndex : mVertices[a].faces) {
            if (FaceContains(mFaces[faceIndex], b)) {
                continue;
            }
            if (!EvaluateFaceNormal(faceIndex, a, b, position, edgeLengthSquared, penalty)) {
                return false;
            }
        }
        for (unsigned int faceIndex : mVertices[b].faces) {
            const FaceData &face = mFaces[faceIndex];
            if (!face.alive || std::find(edgeFaces.begin(), edgeFaces.end(), faceIndex) != edgeFaces.end()) {
                continue;
            }
            if (!EvaluateFaceNormal(faceIndex, a, b, position, edgeLengthSquared, penalty)) {
                return false;
            }
        }
        return true;
    }

    bool EvaluateFaceNormal(unsigned int faceIndex, unsigned int a, unsigned int b,
            const aiVector3D &position, double edgeLengthSquared, double &penalty) const {
        const FaceData &face = mFaces[faceIndex];
        if (!face.alive) {
            return true;
        }
        auto newPosition = [&](unsigned int vertex) {
            return vertex == a || vertex == b ? position : mVertices[vertex].position;
        };
        const aiVector3D p0 = newPosition(face.vertices[0]);
        const aiVector3D p1 = newPosition(face.vertices[1]);
        const aiVector3D p2 = newPosition(face.vertices[2]);
        const aiVector3D cross = (p1 - p0) ^ (p2 - p0);
        if (double(cross.SquareLength()) <= mAreaEpsilonSquared) {
            return true;
        }
        aiVector3D newNormal = cross;
        if (!NormalizeIfPossible(newNormal)) {
            return true;
        }
        const ai_real dot = std::max(ai_real(-1.0), std::min(ai_real(1.0), newNormal * face.normal));
        const ai_real angle = ai_real(1.0) - dot;
        penalty += double(mSettings.mNormalAngleWeight) * 0.05 * double(angle) * double(angle) * edgeLengthSquared;
        return true;
    }

    std::vector<VertexWeight> MergeWeights(unsigned int a, unsigned int b, ai_real t) const {
        std::map<unsigned int, double> merged;
        for (const VertexWeight &weight : mVertices[a].weights) {
            merged[weight.bone] += weight.weight * double(ai_real(1.0) - t);
        }
        for (const VertexWeight &weight : mVertices[b].weights) {
            merged[weight.bone] += weight.weight * double(t);
        }

        std::vector<VertexWeight> result;
        double total = 0.0;
        for (const auto &entry : merged) {
            if (entry.second > 1e-8) {
                result.push_back({ entry.first, entry.second });
                total += entry.second;
            }
        }
        if (total > 1e-12) {
            for (VertexWeight &weight : result) {
                weight.weight /= total;
            }
        }
        return result;
    }

    void Collapse(unsigned int a, unsigned int b, ai_real t, const std::vector<unsigned int> &edgeFaces) {
        VertexData &va = mVertices[a];
        VertexData &vb = mVertices[b];

        va.position = Lerp(va.position, vb.position, t);
        if (mHasNormals) {
            va.normal = Lerp(va.normal, vb.normal, t);
            NormalizeIfPossible(va.normal);
        }
        if (mHasTangents) {
            va.tangent = Lerp(va.tangent, vb.tangent, t);
            va.bitangent = Lerp(va.bitangent, vb.bitangent, t);
            NormalizeIfPossible(va.tangent);
            NormalizeIfPossible(va.bitangent);
        }
        for (unsigned int channel = 0; channel < mUvChannelCount; ++channel) {
            va.texcoords[channel] = Lerp(va.texcoords[channel], vb.texcoords[channel], t);
        }
        for (unsigned int channel = 0; channel < mColorChannelCount; ++channel) {
            va.colors[channel] = Lerp(va.colors[channel], vb.colors[channel], t);
        }
        va.weights = MergeWeights(a, b, t);
        va.quadric.Add(vb.quadric);

        std::vector<unsigned int> affectedFaces = va.faces;
        affectedFaces.insert(affectedFaces.end(), vb.faces.begin(), vb.faces.end());
        std::sort(affectedFaces.begin(), affectedFaces.end());
        affectedFaces.erase(std::unique(affectedFaces.begin(), affectedFaces.end()), affectedFaces.end());

        for (unsigned int faceIndex : vb.faces) {
            FaceData &face = mFaces[faceIndex];
            if (!face.alive) {
                continue;
            }
            if (FaceContains(face, a)) {
                face.alive = false;
                --mFaceCount;
                continue;
            }
            for (unsigned int &vertex : face.vertices) {
                if (vertex == b) {
                    vertex = a;
                    break;
                }
            }
            va.faces.push_back(faceIndex);
        }
        vb.alive = false;
        vb.faces.clear();

        for (unsigned int faceIndex : affectedFaces) {
            FaceData &face = mFaces[faceIndex];
            if (!face.alive) {
                continue;
            }
            if (!FaceHasArea(face)) {
                face.alive = false;
                --mFaceCount;
                continue;
            }
            UpdateFaceNormal(faceIndex);
        }

        ++va.version;
        ++vb.version;
        for (unsigned int faceIndex : va.faces) {
            const FaceData &face = mFaces[faceIndex];
            if (!face.alive) {
                continue;
            }
            PushEdge(face.vertices[0], face.vertices[1]);
            PushEdge(face.vertices[1], face.vertices[2]);
            PushEdge(face.vertices[2], face.vertices[0]);
        }
    }

    bool FaceHasArea(const FaceData &face) const {
        const aiVector3D &a = mVertices[face.vertices[0]].position;
        const aiVector3D &b = mVertices[face.vertices[1]].position;
        const aiVector3D &c = mVertices[face.vertices[2]].position;
        const aiVector3D cross = (b - a) ^ (c - a);
        return double(cross.SquareLength()) > mAreaEpsilonSquared;
    }

    void RebuildMesh() {
        std::vector<unsigned int> vertexMap(mVertexCount, std::numeric_limits<unsigned int>::max());
        std::vector<unsigned int> survivors;
        for (unsigned int i = 0; i < mFaces.size(); ++i) {
            const FaceData &face = mFaces[i];
            if (!face.alive) {
                continue;
            }
            for (unsigned int vertex : face.vertices) {
                if (vertexMap[vertex] == std::numeric_limits<unsigned int>::max()) {
                    vertexMap[vertex] = static_cast<unsigned int>(survivors.size());
                    survivors.push_back(vertex);
                }
            }
        }

        mMesh->mNumVertices = static_cast<unsigned int>(survivors.size());
        delete[] mMesh->mVertices;
        mMesh->mVertices = new aiVector3D[mMesh->mNumVertices];
        for (unsigned int i = 0; i < mMesh->mNumVertices; ++i) {
            mMesh->mVertices[i] = mVertices[survivors[i]].position;
        }

        if (mHasNormals) {
            delete[] mMesh->mNormals;
            mMesh->mNormals = new aiVector3D[mMesh->mNumVertices];
            for (unsigned int i = 0; i < mMesh->mNumVertices; ++i) {
                mMesh->mNormals[i] = mVertices[survivors[i]].normal;
            }
        }
        if (mHasTangents) {
            delete[] mMesh->mTangents;
            delete[] mMesh->mBitangents;
            mMesh->mTangents = new aiVector3D[mMesh->mNumVertices];
            mMesh->mBitangents = new aiVector3D[mMesh->mNumVertices];
            for (unsigned int i = 0; i < mMesh->mNumVertices; ++i) {
                mMesh->mTangents[i] = mVertices[survivors[i]].tangent;
                mMesh->mBitangents[i] = mVertices[survivors[i]].bitangent;
            }
        }
        for (unsigned int channel = 0; channel < mUvChannelCount; ++channel) {
            delete[] mMesh->mTextureCoords[channel];
            mMesh->mTextureCoords[channel] = new aiVector3D[mMesh->mNumVertices];
            for (unsigned int i = 0; i < mMesh->mNumVertices; ++i) {
                mMesh->mTextureCoords[channel][i] = mVertices[survivors[i]].texcoords[channel];
            }
        }
        for (unsigned int channel = 0; channel < mColorChannelCount; ++channel) {
            delete[] mMesh->mColors[channel];
            mMesh->mColors[channel] = new aiColor4D[mMesh->mNumVertices];
            for (unsigned int i = 0; i < mMesh->mNumVertices; ++i) {
                mMesh->mColors[channel][i] = mVertices[survivors[i]].colors[channel];
            }
        }

        delete[] mMesh->mFaces;
        mMesh->mFaces = new aiFace[mFaceCount];
        unsigned int outFace = 0;
        for (unsigned int i = 0; i < mFaces.size(); ++i) {
            const FaceData &face = mFaces[i];
            if (!face.alive) {
                continue;
            }
            aiFace &target = mMesh->mFaces[outFace++];
            target.mNumIndices = 3;
            target.mIndices = new unsigned int[3];
            for (unsigned int j = 0; j < 3; ++j) {
                target.mIndices[j] = vertexMap[face.vertices[j]];
            }
        }
        mMesh->mNumFaces = outFace;

        aiVector3D min, max;
        if (mMesh->mNumVertices == 0) {
            min = max = aiVector3D(ai_real(0.0), ai_real(0.0), ai_real(0.0));
        } else {
            min = max = mMesh->mVertices[0];
            for (unsigned int i = 1; i < mMesh->mNumVertices; ++i) {
                min = std::min(min, mMesh->mVertices[i]);
                max = std::max(max, mMesh->mVertices[i]);
            }
        }
        mMesh->mAABB.mMin = min;
        mMesh->mAABB.mMax = max;

        RebuildBones(survivors);
    }

    void RebuildBones(const std::vector<unsigned int> &survivors) const {
        if (mBoneCount == 0) {
            return;
        }

        std::vector<std::vector<std::pair<unsigned int, double>>> weightsByBone(mBoneCount);
        for (unsigned int i = 0; i < survivors.size(); ++i) {
            for (const VertexWeight &weight : mVertices[survivors[i]].weights) {
                if (weight.bone < mBoneCount) {
                    weightsByBone[weight.bone].emplace_back(i, weight.weight);
                }
            }
        }

        std::vector<aiBone *> newBones;
        newBones.reserve(mBoneCount);
        for (unsigned int boneIndex = 0; boneIndex < mBoneCount; ++boneIndex) {
            if (weightsByBone[boneIndex].empty()) {
                continue;
            }
            const aiBone *source = mMesh->mBones[boneIndex];
            aiBone *bone = new aiBone();
            bone->mName = source->mName;
            bone->mArmature = source->mArmature;
            bone->mNode = source->mNode;
            bone->mOffsetMatrix = source->mOffsetMatrix;
            bone->mNumWeights = static_cast<unsigned int>(weightsByBone[boneIndex].size());
            bone->mWeights = new aiVertexWeight[bone->mNumWeights];
            for (unsigned int i = 0; i < bone->mNumWeights; ++i) {
                bone->mWeights[i].mVertexId = weightsByBone[boneIndex][i].first;
                bone->mWeights[i].mWeight = static_cast<ai_real>(weightsByBone[boneIndex][i].second);
            }
            newBones.push_back(bone);
        }

        for (unsigned int i = 0; i < mMesh->mNumBones; ++i) {
            delete mMesh->mBones[i];
        }
        delete[] mMesh->mBones;
        mMesh->mNumBones = static_cast<unsigned int>(newBones.size());
        mMesh->mBones = newBones.empty() ? nullptr : new aiBone *[newBones.size()];
        for (unsigned int i = 0; i < newBones.size(); ++i) {
            mMesh->mBones[i] = newBones[i];
        }
    }

    aiMesh *mMesh;
    const aiDecimateSettings &mSettings;
    unsigned int mVertexCount;
    unsigned int mFaceCount;
    ai_real mDiagonal;
    double mAreaEpsilonSquared = 0.0;
    bool mHasNormals;
    bool mHasTangents;
    unsigned int mUvChannelCount;
    unsigned int mColorChannelCount;
    unsigned int mBoneCount;

    std::vector<VertexData> mVertices;
    std::vector<FaceData> mFaces;
    std::map<std::pair<unsigned int, unsigned int>,
            std::pair<std::uint64_t, std::uint64_t>> mQueuedEdges;
    std::priority_queue<CollapseCandidate, std::vector<CollapseCandidate>, CandidateCompare> mQueue;
};

bool IsTriangleMesh(const aiMesh *mesh) {
    if (mesh == nullptr || mesh->mNumFaces == 0) {
        return false;
    }
    for (unsigned int i = 0; i < mesh->mNumFaces; ++i) {
        if (mesh->mFaces[i].mNumIndices != 3) {
            return false;
        }
    }
    return true;
}

aiDecimateSettings MakeDefaultSettings() {
    aiDecimateSettings settings;
    settings.mTargetFaceCount = 0;
    settings.mTargetRatio = kDefaultTargetRatio;
    settings.mNormalAngleWeight = kDefaultNormalWeight;
    settings.mUvSeamEpsilon = kDefaultUvSeamEpsilon;
    return settings;
}

} // namespace

// ------------------------------------------------------------------------------------------------
DecimateProcess::DecimateProcess() :
        mSettings(MakeDefaultSettings()) {
    // empty
}

// ------------------------------------------------------------------------------------------------
bool DecimateProcess::IsActive(unsigned int /*pFlags*/) const {
    return false;
}

// ------------------------------------------------------------------------------------------------
bool DecimateProcess::IsActiveEx(aiPostProcessFlags pFlags) const {
    return (pFlags & aiProcess_Decimate) != 0;
}

// ------------------------------------------------------------------------------------------------
void DecimateProcess::SetSettings(const aiDecimateSettings &settings) {
    mSettings = settings;
    if (mSettings.mTargetRatio <= ai_real(0.0)) {
        mSettings.mTargetRatio = kDefaultTargetRatio;
    }
    mSettings.mTargetRatio = std::min(ai_real(1.0), mSettings.mTargetRatio);
    if (mSettings.mNormalAngleWeight < ai_real(0.0)) {
        mSettings.mNormalAngleWeight = kDefaultNormalWeight;
    }
    if (mSettings.mUvSeamEpsilon <= ai_real(0.0)) {
        mSettings.mUvSeamEpsilon = kDefaultUvSeamEpsilon;
    }
}

// ------------------------------------------------------------------------------------------------
void DecimateProcess::SetupProperties(const Importer *pImp) {
    aiDecimateSettings settings = MakeDefaultSettings();
    settings.mTargetFaceCount = static_cast<ai_uint32>(std::max(
            0, pImp->GetPropertyInteger(AI_CONFIG_PP_DECIMATE_TARGET_FACES, 0)));
    settings.mTargetRatio = pImp->GetPropertyFloat(
            AI_CONFIG_PP_DECIMATE_TARGET_RATIO, -ai_real(1.0));
    settings.mNormalAngleWeight = pImp->GetPropertyFloat(
            AI_CONFIG_PP_DECIMATE_NORMAL_ANGLE_WEIGHT, kDefaultNormalWeight);
    settings.mUvSeamEpsilon = pImp->GetPropertyFloat(
            AI_CONFIG_PP_DECIMATE_UV_SEAM_EPSILON, kDefaultUvSeamEpsilon);
    SetSettings(settings);
}

// ------------------------------------------------------------------------------------------------
void DecimateProcess::Execute(aiScene *pScene) {
    if (pScene == nullptr) {
        throw DeadlyImportError("Decimate: null scene");
    }

    std::size_t inputTriangles = 0;
    std::vector<aiMesh *> eligibleMeshes;
    eligibleMeshes.reserve(pScene->mNumMeshes);
    for (unsigned int i = 0; i < pScene->mNumMeshes; ++i) {
        aiMesh *mesh = pScene->mMeshes[i];
        if (!IsTriangleMesh(mesh) || mesh->mNumAnimMeshes != 0 || mesh->mNumFaces < 4) {
            continue;
        }
        inputTriangles += mesh->mNumFaces;
        eligibleMeshes.push_back(mesh);
    }

    if (eligibleMeshes.empty() || inputTriangles == 0) {
        ASSIMP_LOG_DEBUG("Decimate: no triangle meshes to simplify");
        return;
    }

    std::size_t targetTriangles = inputTriangles;
    if (mSettings.mTargetFaceCount != 0) {
        targetTriangles = std::min(inputTriangles, static_cast<std::size_t>(mSettings.mTargetFaceCount));
    } else {
        const ai_real ratio = std::max(ai_real(0.0), std::min(ai_real(1.0), mSettings.mTargetRatio));
        targetTriangles = static_cast<std::size_t>(std::floor(double(inputTriangles) * double(ratio)));
    }
    if (targetTriangles >= inputTriangles) {
        ASSIMP_LOG_DEBUG("Decimate: target is not below the input triangle count");
        return;
    }

    const double ratio = double(targetTriangles) / double(inputTriangles);
    std::vector<unsigned int> meshTargets(eligibleMeshes.size(), 0);
    std::vector<std::pair<double, size_t>> remainders;
    remainders.reserve(eligibleMeshes.size());
    std::size_t allocated = 0;
    for (size_t i = 0; i < eligibleMeshes.size(); ++i) {
        const double exact = double(eligibleMeshes[i]->mNumFaces) * ratio;
        meshTargets[i] = static_cast<unsigned int>(std::floor(exact));
        allocated += meshTargets[i];
        remainders.emplace_back(exact - double(meshTargets[i]), i);
    }
    std::sort(remainders.begin(), remainders.end(),
            [](const std::pair<double, size_t> &lhs, const std::pair<double, size_t> &rhs) {
                return lhs.first > rhs.first;
            });
    std::size_t remaining = targetTriangles - allocated;
    for (size_t i = 0; i < remainders.size() && remaining > 0; ++i, --remaining) {
        ++meshTargets[remainders[i].second];
    }

    std::size_t outputTriangles = 0;
    for (size_t i = 0; i < eligibleMeshes.size(); ++i) {
        aiMesh *mesh = eligibleMeshes[i];
        // Most importers produce per-face vertex copies. Join identical copies
        // first so the decimator has a connected topology to work on; UV and
        // normal splits are retained by JoinVerticesProcess.
#ifndef ASSIMP_BUILD_NO_JOINVERTICES_PROCESS
        JoinVerticesProcess join;
        join.ProcessMesh(mesh, static_cast<unsigned int>(i));
#endif
        MeshDecimator decimator(mesh, mSettings);
        outputTriangles += decimator.Simplify(meshTargets[i]);
    }

    ASSIMP_LOG_DEBUG("Decimate: input triangles ", inputTriangles,
            ", output triangles ", outputTriangles,
            ", target ", targetTriangles);
}

} // namespace Assimp

#endif // !! ASSIMP_BUILD_NO_DECIMATE_PROCESS
