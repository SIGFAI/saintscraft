#pragma once
#include <cstddef>
#include <cstdint>

// Finding game code at runtime by byte signature, so nothing depends on one exact build.
namespace mcsr3::scan
{
	// IDA-style pattern, e.g. "48 89 5C 24 08 ?? 8B". Returns the address of the unique match
	// (+offset), or 0 when there are none or several (logged).
	std::uintptr_t Unique(const char* name, const char* pattern, std::ptrdiff_t offset = 0);

	// Address of a NUL-terminated ASCII string in the image (the whole string, exact), or 0.
	std::uintptr_t String(const char* text);

	// First instruction "lea r64, [rip+disp32]" that loads `target`, or 0.
	std::uintptr_t LeaTo(std::uintptr_t target);

	// Target of a rip-relative operand whose disp32 sits at `dispAt`, for an instruction ending at `next`.
	inline std::uintptr_t Rip(std::uintptr_t dispAt, std::uintptr_t next)
	{
		return next + *reinterpret_cast<const std::int32_t*>(dispAt);
	}

	std::uintptr_t ImageBase();
}
