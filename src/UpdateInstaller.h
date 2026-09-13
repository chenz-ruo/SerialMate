#pragma once

#include "UpdateChecker.h"

#include <string>

bool LaunchUpdateInstaller(const UpdateDownloadResult& update, std::wstring& error);
bool TryRunUpdateInstallerMode(int& exitCode);
