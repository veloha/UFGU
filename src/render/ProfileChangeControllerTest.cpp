

#include "render/ProfileChangeController.hpp"
#include "render/ProfilePreflightRules.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace mfgdlss::render;

namespace
{
int failures = 0;

void check(const bool condition, const char* const what)
{
    if (condition) {
        std::printf("  PASS  %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL  %s\n", what);
    }
}

struct ProductionState
{

    std::uint32_t settings_mode{2};
    std::uint32_t reflex{1};
    std::uint32_t frame_limit{0};
    int sharpness_steps{4};
    bool overlay_enabled{true};
    int ini_writes{};

    std::uint32_t runtime_reflex{1};
    std::uint32_t runtime_frame_limit{0};
    int temporal_history_generation{};
    int transactions_staged{};
    int transactions_begun{};
    int frame_generation_suspends{};
    int frame_generation_ownership_changes{};
    int dlss_contract_mutations{};
    int renderer_resizes{};
    int proxy_recreations{};
    int enb_resets{};

    [[nodiscard]] int total_mutations() const noexcept
    {
        return ini_writes + transactions_staged + transactions_begun +
            frame_generation_suspends + frame_generation_ownership_changes +
            dlss_contract_mutations + renderer_resizes + proxy_recreations +
            enb_resets + temporal_history_generation;
    }
};

struct Ledger
{
    std::vector<std::string> sequence;
    int preflight{};
    int stage_transaction{};
    int apply_runtime_settings{};
    int persist_settings{};
    int reset_history{};
    int reconfigure{};
    int rollback{};
    int report_refusal{};
    std::string last_refusal;

    std::uint32_t preflight_saw_requested_mode{999};
    std::uint32_t settings_mode_at_preflight{999};

    [[nodiscard]] int mutation_callbacks() const noexcept
    {
        return stage_transaction + apply_runtime_settings + persist_settings +
            reset_history + reconfigure + rollback;
    }

    [[nodiscard]] std::string joined() const
    {
        std::string out;
        for (const auto& step : sequence) {
            if (!out.empty()) {
                out += " -> ";
            }
            out += step;
        }
        return out;
    }

    [[nodiscard]] int index_of(const char* const step) const noexcept
    {
        for (std::size_t i = 0; i < sequence.size(); ++i) {
            if (sequence[i] == step) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
};

enum class FailAt
{
    nothing,
    runtime_settings,
    persist,
    reconfigure,
};

ProfileChangeCallbacks make_callbacks(
    ProductionState& production,
    Ledger& ledger,
    const ProfilePreflightInputs& preflight_inputs,
    const FailAt fail_at,
    const std::uint32_t requested_mode)
{
    ProfileChangeCallbacks callbacks{};

    callbacks.preflight =
        [&production, &ledger, preflight_inputs](
            const std::uint32_t mode, const char*& reason) {
            ledger.sequence.emplace_back("preflight");
            ++ledger.preflight;
            ledger.preflight_saw_requested_mode = mode;

            ledger.settings_mode_at_preflight = production.settings_mode;

            static_cast<void>(mode);
            const auto decision = evaluate_profile_preflight(preflight_inputs);
            reason = describe(decision);
            return !is_rejection(decision);
        };

    callbacks.stage_transaction = [&production, &ledger]() {
        ledger.sequence.emplace_back("stage_transaction");
        ++ledger.stage_transaction;
        ++production.transactions_staged;
    };

    callbacks.apply_runtime_settings =
        [&production, &ledger, fail_at]() {
            ledger.sequence.emplace_back("apply_runtime_settings");
            ++ledger.apply_runtime_settings;
            if (fail_at == FailAt::runtime_settings) {
                return false;
            }
            production.runtime_reflex = 2;
            production.runtime_frame_limit = 120;
            return true;
        };

    callbacks.persist_settings =
        [&production, &ledger, fail_at, requested_mode]() {
            ledger.sequence.emplace_back("persist_settings");
            ++ledger.persist_settings;
            if (fail_at == FailAt::persist) {
                return false;
            }
            production.settings_mode = requested_mode;
            production.reflex = 2;
            production.frame_limit = 120;
            production.sharpness_steps = 10;
            production.overlay_enabled = false;
            ++production.ini_writes;
            return true;
        };

    callbacks.reset_history = [&production, &ledger]() {
        ledger.sequence.emplace_back("reset_history");
        ++ledger.reset_history;

        ++production.temporal_history_generation;
    };

    callbacks.reconfigure = [&production, &ledger, fail_at]() {
        ledger.sequence.emplace_back("reconfigure");
        ++ledger.reconfigure;
        ++production.transactions_begun;
        ++production.frame_generation_suspends;
        ++production.frame_generation_ownership_changes;
        if (fail_at == FailAt::reconfigure) {
            return false;
        }
        ++production.dlss_contract_mutations;
        ++production.renderer_resizes;
        ++production.proxy_recreations;
        ++production.enb_resets;
        return true;
    };

    callbacks.rollback = [&production, &ledger]() {
        ledger.sequence.emplace_back("rollback");
        ++ledger.rollback;
        production.settings_mode = 2;
        production.reflex = 1;
        production.frame_limit = 0;
        production.sharpness_steps = 4;
        production.overlay_enabled = true;
        production.runtime_reflex = 1;
        production.runtime_frame_limit = 0;
    };

    callbacks.report_refusal = [&ledger](const char* const reason) {
        ledger.sequence.emplace_back("report_refusal");
        ++ledger.report_refusal;
        ledger.last_refusal = reason != nullptr ? reason : "";
    };

    return callbacks;
}

[[nodiscard]] ProfilePreflightInputs live_quality_4k()
{
    ProfilePreflightInputs in{};
    in.bridge_ready = true;
    in.output_width = 3840;
    in.output_height = 2160;
    in.renderer_available = true;
    in.extent_resolved = true;
    in.requested_width = 2227;
    in.requested_height = 1253;
    in.current_width = 2560;
    in.current_height = 1440;
    in.stable_proxy_identity_published = false;
    in.live_extent_reconfiguration_supported = true;
    in.current_complete_frame_route = true;
    in.requested_complete_frame_route = true;
    return in;
}

[[nodiscard]] ProfilePreflightInputs refusing_state()
{
    auto in = live_quality_4k();
    in.stable_proxy_identity_published = true;
    return in;
}
}

int main()
{
    std::printf(
        "== run_profile_change() sequencing ==\n\n");

    std::printf(
        "-- refused Quality to Balanced request --\n");
    {
        ProductionState production{};
        Ledger ledger{};
        const auto callbacks = make_callbacks(
            production, ledger, refusing_state(), FailAt::nothing, 3);

        ProfileChangeRequest request{};
        request.requested_mode = 3;
        request.active_runtime_mode = 2;

        const auto outcome = run_profile_change(request, callbacks);

        check(
            outcome == ProfileChangeOutcome::refused_by_preflight,
            "the shared controller refuses the request");
        check(
            !mutated_anything(outcome),
            "the outcome reports that nothing was mutated");
        check(
            !succeeded(outcome),
            "the outcome is not a success");
        check(
            ledger.joined() == "preflight -> report_refusal",
            "exactly two callbacks ran, in this order: preflight -> "
            "report_refusal");
        check(
            ledger.mutation_callbacks() == 0,
            "ZERO mutation callbacks were invoked by the real controller");
        check(
            ledger.stage_transaction == 0,
            "SwitchTransaction::stage_request() was never reached");
        check(
            ledger.apply_runtime_settings == 0,
            "the Reflex and frame-limit runtime mutation was never reached");
        check(
            ledger.persist_settings == 0,
            "no Settings mutation and no INI write was reached");
        check(
            ledger.reset_history == 0,
            "UpscalingPass::reset_history() was never reached");
        check(
            ledger.reconfigure == 0,
            "the transaction, MFG suspension and DLSS reconfiguration were "
            "never reached");
        check(
            ledger.rollback == 0,
            "no rollback was needed because nothing was done");
        check(
            production.total_mutations() == 0,
            "the production state records zero mutations of any kind");
        check(
            production.temporal_history_generation == 0,
            "the DLSS temporal history was NOT discarded");
        check(
            production.settings_mode == 2,
            "the persisted mode is still Quality");
        check(
            production.ini_writes == 0,
            "the INI was not written");
        check(
            production.runtime_reflex == 1 &&
                production.runtime_frame_limit == 0,
            "the Reflex mode and frame limit are untouched");
        check(
            production.sharpness_steps == 4 && production.overlay_enabled,
            "Apply is atomic: sharpness and the overlay toggle were NOT "
            "applied under a refused profile request");
        check(
            ledger.report_refusal == 1,
            "the refusal was reported exactly once, for display only");
        check(
            ledger.last_refusal ==
                describe(
                    ProfilePreflightDecision::
                        rejected_proxy_already_published),
            "the reported reason is the expected refusal text");
    }

    std::printf(
        "\n-- the preflight judges its ARGUMENT, never a mutated store --\n");
    {
        ProductionState production{};
        Ledger ledger{};
        const auto callbacks = make_callbacks(
            production, ledger, refusing_state(), FailAt::nothing, 3);
        ProfileChangeRequest request{};
        request.requested_mode = 3;
        request.active_runtime_mode = 2;
        static_cast<void>(run_profile_change(request, callbacks));

        check(
            ledger.preflight_saw_requested_mode == 3,
            "the preflight received the REQUESTED mode (Balanced) as an "
            "argument");
        check(
            ledger.settings_mode_at_preflight == 2,
            "the persisted mode was still Quality when the preflight ran, so "
            "reading Settings there would have judged the wrong value");
        check(
            ledger.preflight_saw_requested_mode !=
                ledger.settings_mode_at_preflight,
            "the argument and the persisted value genuinely differ at that "
            "instant - the old ordering could not tell them apart");
    }

    std::printf("\n-- an accepted change runs every effect, in order --\n");
    {
        const auto inputs = live_quality_4k();

        ProductionState production{};
        Ledger ledger{};
        const auto callbacks =
            make_callbacks(production, ledger, inputs, FailAt::nothing, 3);
        ProfileChangeRequest request{};
        request.requested_mode = 3;
        request.active_runtime_mode = 2;

        const auto outcome = run_profile_change(request, callbacks);

        check(
            outcome == ProfileChangeOutcome::applied,
            "an unpublished proxy allows the same change");
        check(succeeded(outcome), "the outcome is a success");
        check(
            ledger.joined() ==
                "preflight -> stage_transaction -> apply_runtime_settings -> "
                "persist_settings -> reset_history -> reconfigure",
            "the accepted order is preflight, stage, runtime, persist, "
            "reset_history, reconfigure");
        check(
            ledger.index_of("preflight") == 0,
            "the preflight is the FIRST thing that runs");
        check(
            ledger.index_of("reset_history") >
                ledger.index_of("persist_settings"),
            "reset_history() now happens after the change is committed");
        check(
            ledger.index_of("reset_history") < ledger.index_of("reconfigure"),
            "reset_history() still happens before the contract is rebuilt");
        check(
            ledger.index_of("reset_history") > ledger.index_of("preflight"),
            "reset_history() is unreachable until the preflight has accepted");
        check(ledger.rollback == 0, "no rollback on the accepted path");
        check(
            production.temporal_history_generation == 1,
            "the temporal history is discarded exactly once");
        check(
            production.settings_mode == 3,
            "the new mode was persisted");
    }

    std::printf(
        "\n-- an Apply with no profile change still applies the rest --\n");
    {
        ProductionState production{};
        Ledger ledger{};
        const auto callbacks = make_callbacks(
            production, ledger, refusing_state(), FailAt::nothing, 2);
        ProfileChangeRequest request{};
        request.requested_mode = 2;
        request.active_runtime_mode = 2;

        const auto outcome = run_profile_change(request, callbacks);

        check(
            outcome ==
                ProfileChangeOutcome::applied_without_profile_change,
            "a same-mode Apply reports that no profile change was requested");
        check(succeeded(outcome), "it is still a successful Apply");
        check(
            ledger.preflight == 0,
            "the preflight is not consulted when there is nothing to change");
        check(
            ledger.stage_transaction == 0,
            "no transaction is staged for a same-mode Apply (the '3 -> 3' "
            "transaction that could not undo itself)");
        check(
            ledger.reset_history == 0,
            "the temporal history is not discarded for a same-mode Apply");
        check(
            ledger.reconfigure == 0,
            "no reconfiguration is requested for a same-mode Apply");
        check(
            ledger.persist_settings == 1 && production.ini_writes == 1,
            "the sharpness, Reflex, frame-limit and overlay selections were "
            "still applied");
    }

    std::printf("\n-- failures after acceptance roll back --\n");
    {
        const auto inputs = live_quality_4k();

        ProductionState production{};
        Ledger ledger{};
        const auto callbacks = make_callbacks(
            production, ledger, inputs, FailAt::runtime_settings, 3);
        ProfileChangeRequest request{};
        request.requested_mode = 3;
        request.active_runtime_mode = 2;
        const auto outcome = run_profile_change(request, callbacks);

        check(
            outcome == ProfileChangeOutcome::failed_runtime_settings,
            "a refusing frame-generation runtime is reported distinctly");
        check(ledger.rollback == 1, "it rolled back");
        check(
            ledger.persist_settings == 0,
            "nothing was persisted after a runtime refusal");
        check(
            ledger.reset_history == 0,
            "the temporal history survived a runtime refusal");
        check(
            ledger.reconfigure == 0,
            "no reconfiguration was attempted after a runtime refusal");
    }
    {
        const auto inputs = live_quality_4k();

        ProductionState production{};
        Ledger ledger{};
        const auto callbacks =
            make_callbacks(production, ledger, inputs, FailAt::persist, 3);
        ProfileChangeRequest request{};
        request.requested_mode = 3;
        request.active_runtime_mode = 2;
        const auto outcome = run_profile_change(request, callbacks);

        check(
            outcome == ProfileChangeOutcome::failed_persist,
            "a refusing persistence layer is reported distinctly");
        check(ledger.rollback == 1, "it rolled back");
        check(
            ledger.reset_history == 0,
            "the temporal history survived a persistence failure");
        check(
            production.temporal_history_generation == 0,
            "and the production state confirms it");
        check(
            ledger.reconfigure == 0,
            "no reconfiguration was attempted after a persistence failure");
    }
    {
        const auto inputs = live_quality_4k();

        ProductionState production{};
        Ledger ledger{};
        const auto callbacks =
            make_callbacks(production, ledger, inputs, FailAt::reconfigure, 3);
        ProfileChangeRequest request{};
        request.requested_mode = 3;
        request.active_runtime_mode = 2;
        const auto outcome = run_profile_change(request, callbacks);

        check(
            outcome == ProfileChangeOutcome::failed_reconfiguration,
            "a refusing renderer is reported distinctly");
        check(
            ledger.reset_history == 1,
            "the history had already been discarded, which is legitimate here "
            "because the change was genuinely proceeding");
        check(ledger.rollback == 1, "it rolled back");
        check(
            production.settings_mode == 2,
            "the rollback restored the persisted mode");
    }

    std::printf("\n-- an incomplete callback set does nothing at all --\n");
    {
        ProductionState production{};
        Ledger ledger{};
        auto callbacks = make_callbacks(
            production, ledger, refusing_state(), FailAt::nothing, 3);
        callbacks.reset_history = nullptr;

        ProfileChangeRequest request{};
        request.requested_mode = 3;
        request.active_runtime_mode = 2;
        const auto outcome = run_profile_change(request, callbacks);

        check(
            outcome == ProfileChangeOutcome::aborted_incomplete_callbacks,
            "a missing callback aborts instead of throwing bad_function_call");
        check(
            !mutated_anything(outcome),
            "the outcome reports that nothing was mutated");
        check(
            ledger.sequence.empty(),
            "not even the preflight ran");
        check(
            production.total_mutations() == 0,
            "the production state records zero mutations");
    }

    std::printf(
        "\n-- every rejecting condition reaches zero effects --\n");
    {
        struct Case
        {
            const char* name;
            ProfilePreflightInputs inputs;
        };
        auto no_bridge = live_quality_4k();
        no_bridge.bridge_ready = false;
        auto no_output = live_quality_4k();
        no_output.output_width = 0;
        no_output.output_height = 0;
        auto no_renderer = live_quality_4k();
        no_renderer.renderer_available = false;
        auto unresolved = live_quality_4k();
        unresolved.extent_resolved = false;
        auto zero_extent = live_quality_4k();
        zero_extent.requested_width = 0;
        auto wider = live_quality_4k();
        wider.requested_width = 4000;
        auto taller = live_quality_4k();
        taller.requested_height = 2200;
        auto no_active_extent = live_quality_4k();
        no_active_extent.current_width = 0;
        auto published = live_quality_4k();
        published.stable_proxy_identity_published = true;
        auto unsupported = live_quality_4k();
        unsupported.live_extent_reconfiguration_supported = false;
        auto route_change = live_quality_4k();
        route_change.requested_width = route_change.current_width;
        route_change.requested_height = route_change.current_height;
        route_change.requested_complete_frame_route = false;

        const Case cases[] = {
            {"no presentation bridge", no_bridge},
            {"no output extent", no_output},
            {"Skyrim's renderer singleton unavailable", no_renderer},
            {"requested extent could not be resolved", unresolved},
            {"requested extent is zero", zero_extent},
            {"requested extent wider than the output", wider},
            {"requested extent taller than the output", taller},
            {"active extent unavailable", no_active_extent},
            {"stable proxy already published", published},
            {"live extent reconfiguration unavailable", unsupported},
            {"presentation route change requires restart", route_change},
        };

        for (const auto& one : cases) {
            ProductionState production{};
            Ledger ledger{};
            const auto callbacks = make_callbacks(
                production, ledger, one.inputs, FailAt::nothing, 3);
            ProfileChangeRequest request{};
            request.requested_mode = 3;
            request.active_runtime_mode = 2;
            const auto outcome = run_profile_change(request, callbacks);

            const auto clean =
                outcome == ProfileChangeOutcome::refused_by_preflight &&
                ledger.mutation_callbacks() == 0 &&
                production.total_mutations() == 0;
            check(clean, one.name);
        }
    }

    std::printf("\n-- every outcome describes itself --\n");
    {
        const ProfileChangeOutcome all[] = {
            ProfileChangeOutcome::applied,
            ProfileChangeOutcome::applied_without_profile_change,
            ProfileChangeOutcome::refused_by_preflight,
            ProfileChangeOutcome::failed_runtime_settings,
            ProfileChangeOutcome::failed_persist,
            ProfileChangeOutcome::failed_reconfiguration,
            ProfileChangeOutcome::aborted_incomplete_callbacks,
        };
        auto distinct = true;
        for (const auto outer : all) {
            if (describe(outer) == nullptr ||
                std::string{describe(outer)}.empty()) {
                distinct = false;
            }
            for (const auto inner : all) {
                if (outer != inner &&
                    std::string{describe(outer)} ==
                        std::string{describe(inner)}) {
                    distinct = false;
                }
            }
        }
        check(distinct, "all seven outcomes have distinct, non-empty text");
        check(
            !mutated_anything(ProfileChangeOutcome::refused_by_preflight),
            "refused_by_preflight is classified as non-mutating");
        check(
            mutated_anything(ProfileChangeOutcome::failed_reconfiguration),
            "failed_reconfiguration is classified as mutating");
    }

    std::printf(
        "\n%s\n",
        failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
