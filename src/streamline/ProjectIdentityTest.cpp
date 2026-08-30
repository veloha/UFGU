#include "streamline/ProjectIdentity.hpp"

#include <string_view>

using namespace mfgdlss::streamline;

int main()
{
    int failures{};
    const auto check = [&failures](const bool condition) {
        failures += condition ? 0 : 1;
    };

    static_assert(
        kProjectId == std::string_view{"d514d00c-85c7-4d05-9516-53b6528d9a6b"},
        "the Streamline project id is registered with NVIDIA and must not "
        "change without re-registering it");

    check(kProjectId.size() == 36U);
    check(kProjectId[8] == '-');
    check(kProjectId[13] == '-');
    check(kProjectId[18] == '-');
    check(kProjectId[23] == '-');

    const auto is_hex_or_dash = [](const char character) {
        return character == '-' ||
            (character >= '0' && character <= '9') ||
            (character >= 'a' && character <= 'f');
    };
    for (const auto character : kProjectId) {
        check(is_hex_or_dash(character));
    }

    check(kProjectId.data()[kProjectId.size()] == '\0');
    check(kEngineVersionString.data()[kEngineVersionString.size()] == '\0');

    check(!kEngineVersionString.empty());
    check(kEngineVersionString.find("UFGU-") != std::string_view::npos);

    return failures == 0 ? 0 : 1;
}
