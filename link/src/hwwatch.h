#pragma once
#include <cstdint>

namespace mcsr3::hwwatch
{
	void Set(std::uintptr_t addr);  // 0 clears
	int Hits();
}
