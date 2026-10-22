#pragma once
#include <cstdint>
namespace TlsDiag {
struct Failure { uint32_t result; const char* operation; };
void clear();
Failure lastFailure();
}
