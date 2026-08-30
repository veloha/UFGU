#include "render/FrameGenerationChoicePolicy.hpp"

#include <array>

using namespace mfgdlss::render;

int main()
{
    int failures{};
    const auto check = [&failures](const bool condition) {
        failures += condition ? 0 : 1;
    };

    constexpr std::array choices{0U, 2U, 3U, 4U, 6U};

    check(usable_choice_count(choices, 0U) == choices.size());

    check(usable_choice_count(choices, 1U) == 1U);
    check(usable_choice_count(choices, 2U) == 2U);
    check(usable_choice_count(choices, 3U) == 3U);
    check(usable_choice_count(choices, 4U) == 4U);
    check(usable_choice_count(choices, 6U) == 5U);

    check(usable_choice_count(choices, 5U) == 4U);
    check(usable_choice_count(choices, 7U) == 5U);
    check(usable_choice_count(choices, 64U) == 5U);

    constexpr std::array<unsigned, 0> empty{};
    check(usable_choice_count(empty, 4U) == 0U);

    constexpr std::array single{0U};
    check(usable_choice_count(single, 4U) == 1U);

    return failures;
}
