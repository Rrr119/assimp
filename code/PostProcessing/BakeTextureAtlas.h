#pragma once
#ifndef ASSIMP_BUILD_NO_BAKETEXTUREATLAS_PROCESS

#include "Common/BaseProcess.h"

#include <cstddef>
#include <string>

struct aiScene;
class IOSystem;

namespace Assimp {

class Importer;

class ASSIMP_API BakeTextureAtlasProcess final : public BaseProcess {
public:
    BakeTextureAtlasProcess();
    ~BakeTextureAtlasProcess() override = default;

    bool IsActive(aiPostProcessFlags pFlags) const override;
    void SetupProperties(const Importer *pImp) override;
    void Execute(aiScene *pScene) override;

private:
    std::size_t mMaxAtlasSize;
    std::size_t mPadding;
    std::string mOutputDirectory;
    std::string mAtlasFilename;
    std::string mRootPath;
    IOSystem *mIOHandler;
};

} // namespace Assimp

#endif // ASSIMP_BUILD_NO_BAKETEXTUREATLAS_PROCESS
