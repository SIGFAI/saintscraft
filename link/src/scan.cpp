#include "scan.h"

#include "log.h"

#include <windows.h>
#include <psapi.h>

#include <cstring>
#include <vector>

namespace mcsr3::scan
{
	namespace
	{
		struct Image
		{
			const std::uint8_t* base;
			std::size_t size;
		};

		const Image& Img()
		{
			static const Image img = [] {
				MODULEINFO mi{};
				GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &mi, sizeof(mi));
				return Image{ static_cast<const std::uint8_t*>(mi.lpBaseOfDll), mi.SizeOfImage };
			}();
			return img;
		}

		// Readable parts of the image only: the gaps between sections are not committed.
		template <class F>
		void ForEachSection(F&& f)
		{
			auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Img().base);
			auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(Img().base + dos->e_lfanew);
			auto* sec = IMAGE_FIRST_SECTION(nt);
			for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
				f(Img().base + sec->VirtualAddress, std::size_t(sec->Misc.VirtualSize), sec->Characteristics);
		}
	}

	std::uintptr_t ImageBase() { return reinterpret_cast<std::uintptr_t>(Img().base); }

	std::uintptr_t Unique(const char* name, const char* pattern, std::ptrdiff_t offset)
	{
		std::vector<int> pat;  // -1 = wildcard
		for (const char* p = pattern; *p;)
		{
			if (*p == ' ')
				p++;
			else if (*p == '?')
			{
				pat.push_back(-1);
				p += p[1] == '?' ? 2 : 1;
			}
			else
			{
				pat.push_back(int(std::strtoul(p, nullptr, 16)));
				p += 2;
			}
		}
		std::uintptr_t found = 0;
		int hits = 0;
		ForEachSection([&](const std::uint8_t* s, std::size_t n, DWORD ch) {
			if (!(ch & IMAGE_SCN_MEM_EXECUTE) || n < pat.size())
				return;
			for (std::size_t i = 0; i + pat.size() <= n; i++)
			{
				std::size_t j = 0;
				while (j < pat.size() && (pat[j] < 0 || s[i + j] == pat[j]))
					j++;
				if (j == pat.size())
				{
					if (!hits++)
						found = reinterpret_cast<std::uintptr_t>(s + i) + offset;
				}
			}
		});
		if (hits != 1)
		{
			Log("scan: %s: %d matches (want 1)", name, hits);
			return 0;
		}
		Log("scan: %s = SRTTR+0x%llx", name, (unsigned long long)(found - ImageBase()));
		return found;
	}

	std::uintptr_t String(const char* text)
	{
		std::size_t len = std::strlen(text) + 1;  // with the NUL
		std::uintptr_t found = 0;
		ForEachSection([&](const std::uint8_t* s, std::size_t n, DWORD ch) {
			if (found || (ch & IMAGE_SCN_MEM_EXECUTE))
				return;
			for (std::size_t i = 1; i + len <= n; i++)
				if (s[i - 1] == 0 && std::memcmp(s + i, text, len) == 0)
				{
					found = reinterpret_cast<std::uintptr_t>(s + i);
					return;
				}
		});
		return found;
	}

	std::uintptr_t LeaTo(std::uintptr_t target)
	{
		std::uintptr_t found = 0;
		ForEachSection([&](const std::uint8_t* s, std::size_t n, DWORD ch) {
			if (found || !(ch & IMAGE_SCN_MEM_EXECUTE))
				return;
			for (std::size_t i = 0; i + 7 <= n; i++)
			{
				// REX.W (48/4C) 8D modrm(mod=00, rm=101: rip-relative)
				if ((s[i] == 0x48 || s[i] == 0x4C) && s[i + 1] == 0x8D && (s[i + 2] & 0xC7) == 0x05)
				{
					auto at = reinterpret_cast<std::uintptr_t>(s + i);
					if (Rip(at + 3, at + 7) == target)
					{
						found = at;
						return;
					}
				}
			}
		});
		return found;
	}
}
