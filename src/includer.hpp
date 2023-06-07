#pragma once

#include <glslang/Public/ShaderLang.h>
#include "workspace.hpp"

class FileIncluder : public glslang::TShader::Includer {
    Workspace* workspace;

public:
    FileIncluder(Workspace* workspace) : workspace(workspace) {}

    virtual void releaseInclude(IncludeResult*) override;

    virtual IncludeResult* includeLocal(
            const char* header_name,
            const char* includer_name,
            size_t depth) override;
};

