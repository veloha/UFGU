

#include "render/ApplyOutcome.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
int failures{};

void check(const bool condition, const char* const description)
{
    std::printf("  %s  %s\n", condition ? "PASS" : "FAIL", description);
    failures += condition ? 0 : 1;
}

using mfgdlss::render::ApplyOutcome;

constexpr ApplyOutcome kAll[] = {
    ApplyOutcome::none,
    ApplyOutcome::committed,
    ApplyOutcome::staged_for_restart,
    ApplyOutcome::refused,
    ApplyOutcome::failed,
    ApplyOutcome::rolled_back,
};

[[nodiscard]] bool contains(const wchar_t* const text, const wchar_t* const needle)
{
    return std::wcsstr(text, needle) != nullptr;
}
}

int main()
{
    using mfgdlss::render::banner_prefix;
    using mfgdlss::render::is_accepted;
    using mfgdlss::render::is_flagged;
    using mfgdlss::render::may_persist_configuration;
    using mfgdlss::render::mutates_active_renderer;
    using mfgdlss::render::outcome_name;

    std::printf("the field defect: staged must never read as refused\n");
    check(
        !contains(banner_prefix(ApplyOutcome::staged_for_restart), L"REFUSED"),
        "a staged restart never renders the word REFUSED");
    check(
        std::wcscmp(
            banner_prefix(ApplyOutcome::staged_for_restart),
            L"RESTART REQUIRED: ") == 0,
        "a staged restart says exactly RESTART REQUIRED");
    check(
        is_accepted(ApplyOutcome::staged_for_restart),
        "a staged restart is an accepted outcome");
    check(
        !is_flagged(ApplyOutcome::staged_for_restart),
        "a staged restart is not drawn as a problem");
    check(
        !mutates_active_renderer(ApplyOutcome::staged_for_restart),
        "a staged restart does not touch the running renderer");

    std::printf("only a completed outcome may leave the INI changed\n");
    check(
        !may_persist_configuration(ApplyOutcome::refused),
        "a refusal may not persist configuration");
    check(
        !may_persist_configuration(ApplyOutcome::failed),
        "a failure may not persist configuration");
    check(
        !may_persist_configuration(ApplyOutcome::rolled_back),
        "a rollback may not persist configuration");
    check(
        may_persist_configuration(ApplyOutcome::committed),
        "a committed change persists");

    std::printf("only a true refusal says REFUSED\n");
    for (const auto outcome : kAll) {
        if (outcome == ApplyOutcome::refused) {
            continue;
        }
        check(
            !contains(banner_prefix(outcome), L"REFUSED"),
            (std::string{"no REFUSED in the banner for "} +
             outcome_name(outcome))
                .c_str());
    }
    check(
        contains(banner_prefix(ApplyOutcome::refused), L"REFUSED"),
        "a refusal does say REFUSED");

    std::printf("every state is distinguishable\n");
    {
        std::vector<std::wstring> prefixes;
        std::vector<std::string> names;
        for (const auto outcome : kAll) {
            prefixes.emplace_back(banner_prefix(outcome));
            names.emplace_back(outcome_name(outcome));
        }
        auto unique = true;
        for (std::size_t i = 0; i < prefixes.size(); ++i) {
            for (std::size_t j = i + 1; j < prefixes.size(); ++j) {
                if (prefixes[i] == prefixes[j] || names[i] == names[j]) {
                    unique = false;
                }
            }
        }
        check(unique, "no two outcomes share a prefix or a name");
        check(
            prefixes[0].empty(),
            "the neutral state draws no banner");
        for (std::size_t i = 1; i < prefixes.size(); ++i) {
            check(
                !prefixes[i].empty() &&
                    prefixes[i].find(L": ") != std::wstring::npos,
                (std::string{"a reportable state has a delimited prefix: "} +
                 names[i])
                    .c_str());
        }
    }

    std::printf("failure and rollback are flagged, acceptance is not\n");
    check(
        is_flagged(ApplyOutcome::refused) && is_flagged(ApplyOutcome::failed) &&
            is_flagged(ApplyOutcome::rolled_back),
        "problems are flagged");
    check(
        !is_flagged(ApplyOutcome::committed) &&
            !is_flagged(ApplyOutcome::staged_for_restart) &&
            !is_flagged(ApplyOutcome::none),
        "successes and the neutral state are not flagged");
    check(
        mutates_active_renderer(ApplyOutcome::committed) &&
            mutates_active_renderer(ApplyOutcome::rolled_back) &&
            !mutates_active_renderer(ApplyOutcome::refused) &&
            !mutates_active_renderer(ApplyOutcome::failed),
        "renderer mutation is reported for exactly commit and rollback");

    std::printf("ApplyOutcomeTest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
