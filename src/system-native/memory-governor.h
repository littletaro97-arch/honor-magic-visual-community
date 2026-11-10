#pragma once
#include "system.h"
#include <tlhelp32.h>

namespace magic::system
{
// Deep module: one interface owns memory-list cleaning and verified child-process freezing.
// Callers must use the ordinary control channel; failures are returned as structured JSON.
class MemoryGovernor
{
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    MemoryGovernor();
    ~MemoryGovernor();
    MemoryGovernor(MemoryGovernor const &) = delete;
    MemoryGovernor &operator=(MemoryGovernor const &) = delete;

    JsonObject Health() const;
    JsonObject Clean(JsonObject const &arguments);
    // Freeze accepts pid:Number, image:String, created:decimal FILETIME String.
    JsonObject Freeze(JsonObject const &arguments, DWORD parentPid, HANDLE parent);
    JsonObject Resume(JsonObject const &arguments);
    void ResumeAll();
};
} // namespace magic::system
