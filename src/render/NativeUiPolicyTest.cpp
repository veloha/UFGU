#include "render/NativeUiPolicy.hpp"

int main()
{
    using mfgdlss::render::make_native_ui_policy;
    int failures = 0;
    const auto check = [&failures](const bool condition) {
        failures += condition ? 0 : 1;
    };

    const auto native_disabled = make_native_ui_policy(false, false, false);
    check(!native_disabled.install_scaleform_boundary);
    check(!native_disabled.capture_at_scaleform_boundary);
    check(!native_disabled.service_at_present);

    const auto native_enabled = make_native_ui_policy(true, false, false);
    check(native_enabled.install_scaleform_boundary);
    check(native_enabled.capture_at_scaleform_boundary);
    check(native_enabled.service_at_present);

    const auto disabled_mid_frame = make_native_ui_policy(false, false, true);
    check(!disabled_mid_frame.capture_at_scaleform_boundary);
    check(disabled_mid_frame.service_at_present);

    const auto virtual_enabled = make_native_ui_policy(true, true, false);
    check(virtual_enabled.install_scaleform_boundary);
    check(!virtual_enabled.capture_at_scaleform_boundary);
    check(!virtual_enabled.service_at_present);

    return failures;
}
