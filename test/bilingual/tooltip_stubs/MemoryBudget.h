#pragma once
#include <cstdint>
namespace MemoryBudget {
struct HeapSnapshot {
  uint32_t freeHeap;
  uint32_t maxAllocHeap;
};
inline HeapSnapshot snapshot() { return {200 * 1024, 100 * 1024}; }
}  // namespace MemoryBudget
