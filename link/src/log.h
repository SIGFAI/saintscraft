#pragma once
#include <string>

namespace mcsr3
{
	// Folder next to SRTTR.exe that holds our log and dev-channel files ("<game>\mcsr3\").
	const std::wstring& DataDir();
	void Log(const char* fmt, ...);
}
