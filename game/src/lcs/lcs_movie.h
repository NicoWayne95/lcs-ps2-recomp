#pragma once
#include <string>
#include <string_view>
class PS2Runtime;

namespace lcs_movie {
bool play(PS2Runtime& runtime, std::string_view name, unsigned skipAfter);
void draw(int skipGamepad);
std::string activeName();
void shutdown();
}
