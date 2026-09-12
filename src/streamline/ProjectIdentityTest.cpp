#include "streamline/ProjectIdentity.hpp"

#include <string>
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

    const auto se = engine_version_string(1, 5, 97, 0, "0.1.0");
    const auto ae = engine_version_string(1, 6, 1170, 0, "0.1.0");
    check(se == "SkyrimSE-1.5.97 UFGU-0.1.0");
    check(ae == "SkyrimSE-1.6.1170 UFGU-0.1.0");
    check(se != ae);
    check(engine_version_string(1, 6, 1170, 3, "0.2.0") ==
          "SkyrimSE-1.6.1170.3 UFGU-0.2.0");
    check(se.c_str()[se.size()] == '\0');
    check(se.find("UFGU-") != std::string::npos);
    check(se.rfind(kEngineName, 0) == 0);

    return failures == 0 ? 0 : 1;
}
