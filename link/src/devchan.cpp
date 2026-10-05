// Dev channel: drop <game>\mcsr3\exec\<name>.lua and it runs in the gameplay Lua state on the game
// thread; the result lands in <name>.out ("ok"/"error" line, then everything mclog() printed).
// tools/srlua.py is the client.
#include "devchan.h"

#include "log.h"
#include "lua.h"

#include <windows.h>

#include <fstream>
#include <sstream>
#include <thread>

namespace mcsr3::devchan
{
	void Start()
	{
		std::thread([] {
			const std::wstring dir = DataDir() + L"exec\\";
			CreateDirectoryW(dir.c_str(), nullptr);
			for (;;)
			{
				Sleep(50);
				WIN32_FIND_DATAW fd;
				HANDLE h = FindFirstFileW((dir + L"*.lua").c_str(), &fd);
				if (h == INVALID_HANDLE_VALUE)
					continue;
				do
				{
					std::wstring path = dir + fd.cFileName;
					std::ifstream in(path, std::ios::binary);
					std::stringstream ss;
					ss << in.rdbuf();
					in.close();
					if (!DeleteFileW(path.c_str()))
						continue;  // still being written: next round
					std::wstring stem = path.substr(0, path.size() - 4);
					std::string name;  // ASCII names only
					for (auto it = stem.begin() + dir.size(); it != stem.end(); ++it)
						name += char(*it);
					// Written to .tmp then renamed, so the client never reads a half-written file.
					lua::Run(name, ss.str(), [stem](bool ok, const std::string& out) {
						std::wstring tmp = stem + L".tmp";
						{
							std::ofstream o(tmp, std::ios::binary);
							o << (ok ? "ok\n" : "error\n") << out;
						}
						MoveFileExW(tmp.c_str(), (stem + L".out").c_str(), MOVEFILE_REPLACE_EXISTING);
					});
				} while (FindNextFileW(h, &fd));
				FindClose(h);
			}
		}).detach();
	}
}
