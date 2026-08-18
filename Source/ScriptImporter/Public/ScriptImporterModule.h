#pragma once

#include "Modules/ModuleManager.h"

class FScriptImporterModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void AddMenuEntry(class FMenuBuilder& MenuBuilder);
    void OpenImportDialog();
};
