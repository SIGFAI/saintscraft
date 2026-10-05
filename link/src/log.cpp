#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace mcsr3
{
	const std::wstring& DataDir()
	{
		static const std::wstring dir = [] {
			wchar_t exe[MAX_PATH];
			GetModuleFileNameW(nullptr, exe, MAX_PATH);
			std::wstring d = exe;
			d.resize(d.find_last_of(L"\\/") + 1);
			d += L"mcsr3\\";
			CreateDirectoryW(d.c_str(), nullptr);
			return d;
		}();
		return dir;
	}

	void Log(const char* fmt, ...)
	{
		static std::mutex m;
		static FILE* f;
		std::lock_guard lock(m);
		if (!f)
		{
			f = _wfopen((DataDir() + L"mcsr3.log").c_str(), L"w");
			if (!f)
				return;
		}
		SYSTEMTIME t;
		GetLocalTime(&t);
		std::fprintf(f, "%02d:%02d:%02d.%03d [%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId());
		va_list ap;
		va_start(ap, fmt);
		std::vfprintf(f, fmt, ap);
		va_end(ap);
		std::fputc('\n', f);
		std::fflush(f);
	}
}
