#include "render/ProfileChangeController.hpp"

namespace mfgdlss::render
{

const char* describe(const ProfileChangeOutcome outcome) noexcept
{
    switch (outcome) {
    case ProfileChangeOutcome::applied:
        return "the profile change was accepted and applied";
    case ProfileChangeOutcome::applied_without_profile_change:
        return "no profile change was requested; the other selections were "
               "applied";
    case ProfileChangeOutcome::refused_by_preflight:
        return "the profile change was refused before anything was changed";
    case ProfileChangeOutcome::failed_runtime_settings:
        return "the frame-generation runtime refused the request; rolled back";
    case ProfileChangeOutcome::failed_persist:
        return "the selection could not be persisted; rolled back";
    case ProfileChangeOutcome::failed_reconfiguration:
        return "the renderer refused the reconfiguration; rolled back";
    case ProfileChangeOutcome::aborted_incomplete_callbacks:
        return "the profile-change controller was given an incomplete callback "
               "set and did nothing";
    }
    return "unknown";
}

bool mutated_anything(const ProfileChangeOutcome outcome) noexcept
{
    switch (outcome) {
    case ProfileChangeOutcome::refused_by_preflight:
    case ProfileChangeOutcome::aborted_incomplete_callbacks:
        return false;
    case ProfileChangeOutcome::applied:
    case ProfileChangeOutcome::applied_without_profile_change:
    case ProfileChangeOutcome::failed_runtime_settings:
    case ProfileChangeOutcome::failed_persist:
    case ProfileChangeOutcome::failed_reconfiguration:
        return true;
    }
    return true;
}

bool succeeded(const ProfileChangeOutcome outcome) noexcept
{
    return outcome == ProfileChangeOutcome::applied ||
           outcome == ProfileChangeOutcome::applied_without_profile_change;
}

ProfileChangeOutcome run_profile_change(
    const ProfileChangeRequest& request,
    const ProfileChangeCallbacks& callbacks)
{

    if (!callbacks.preflight ||
        !callbacks.stage_transaction ||
        !callbacks.apply_runtime_settings ||
        !callbacks.persist_settings ||
        !callbacks.reset_history ||
        !callbacks.reconfigure ||
        !callbacks.rollback ||
        !callbacks.report_refusal) {
        return ProfileChangeOutcome::aborted_incomplete_callbacks;
    }

    const auto profile_change_requested =
        request.requested_mode != request.active_runtime_mode;

    if (profile_change_requested) {
        const char* reason = "the profile change was refused";
        if (!callbacks.preflight(request.requested_mode, reason)) {

            callbacks.report_refusal(reason);
            return ProfileChangeOutcome::refused_by_preflight;
        }

        callbacks.stage_transaction();
    }

    if (!callbacks.apply_runtime_settings()) {
        callbacks.rollback();
        return ProfileChangeOutcome::failed_runtime_settings;
    }

    if (!callbacks.persist_settings()) {
        callbacks.rollback();
        return ProfileChangeOutcome::failed_persist;
    }

    if (!profile_change_requested) {
        return ProfileChangeOutcome::applied_without_profile_change;
    }

    callbacks.reset_history();

    if (!callbacks.reconfigure()) {
        callbacks.rollback();
        return ProfileChangeOutcome::failed_reconfiguration;
    }

    return ProfileChangeOutcome::applied;
}

}
