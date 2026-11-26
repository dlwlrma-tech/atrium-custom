# Contributing to Atrium Desktop

Thanks for your interest. The most valuable contributions to Atrium right now are:

- **Reports from other machines**: stable Windows 11 builds, multiple monitors, HDR, high refresh rates, desktop icons enabled, AMD/NVIDIA-only systems. Describe what you saw and attach `%LOCALAPPDATA%\Atrium\logs\core.log` to an issue.
- **Design review**: the design language is in [docs/DESIGN.md](docs/DESIGN.md).
- **Compatibility reports** for tray icons, task grouping and Show Desktop behaviour with specific applications.

## Ground rules

- Atrium must never require disabling security features, replacing system files, injecting into Explorer or DWM, or killing Explorer. Proposals that depend on these will not be merged into the core.
- Prefer public, documented Windows APIs. If an undocumented behaviour is unavoidable, isolate it behind a compatibility module and explain it in the pull request.
- Report results honestly: a test is `PASS`, `FAIL` or `SKIP`. Missing hardware is a skip, not "should work".
- Do not commit screenshots or data that show other people's windows, notifications or personal content. Use `ATRIUM_PRIVACY=1` and a synthetic backdrop for images.

## Architecture at a glance

- **Native core (`native/core/`)**: strictly one-way layers enforced by `tools/lint.ps1` (no include cycles):
  - **Foundation headers** (`settings.h`, `bar_edge.h`, `clock.h`, `log.h`, `localization.h`, `win32_path.h`, etc.) — pure helpers and data contracts, free of WinRT/DirectX headers.
  - **`render/`** (DirectX/D2D glass, backdrop capture, `CapturePolicy`) and **`shell/`** (task model, tray, displays, system status) — independent siblings; neither includes the other.
  - **`ui/`** (popups, control center, stage manager) — builds on foundation, `render/`, and `shell/`.
  - **Composition root** (`bar.cpp`, `main.cpp`, `settings_server.cpp`) — wires the bar window, render thread, and UI together.
- **Settings app (`settings/Atrium.Settings/`)**: pure C# model/logic files (`BarSettings.cs`, `SettingsSession.cs`, `CoreClient.cs`, `CornerProfile.cs`, `LocalizationValidation.cs`, etc.) stay free of WinUI/UI namespaces (`lint.ps1` enforces this too).
- **Adding or changing a setting**: update all four places together — `native/core/settings.{h,cpp}`, `settings/Atrium.Settings/BarSettings.cs`, `docs/SETTINGS-PROTOCOL.md`, and `tests/fixtures/settings-schema.json` (`SettingsChecks` cross-checks them automatically).
- **Adding UI strings**: update every locale file in `locales/*.json` (`LocalizationChecks` and `tools/check-localization.ps1` verify completeness and format specifiers).

## Local checks

Run these from the repository root before opening a pull request:

```powershell
# Build core, Settings, tests, and portable zip
./tools/build.ps1 -Zip -Tests -Ninja

# Native unit tests & settings / localization checks
ctest --test-dir native/build/win-x64 --output-on-failure --timeout 60
dotnet run --project tests/SettingsChecks/SettingsChecks.csproj -c Release -- --core-normalizer native/build/win-x64/tests/display_settings_tests.exe
dotnet run --project tests/LocalizationChecks -c Release
./tools/check-localization.ps1
./tools/tests/check-localization.tests.ps1

# Lint & script checks (add -Base origin/main for PR diffs)
./tools/lint.ps1 -Tidy -DotNet -BuildDir native/build/win-x64
./tools/tests/lint.tests.ps1
```

If your change touches `Update.ps1`, `Uninstall.ps1`, or packaging scripts, also run:

```powershell
./tools/tests/update.tests.ps1            # optional: -CandidateZipPath <zip>
./tools/tests/uninstall.tests.ps1
./tools/verify-package.ps1 -ZipPath <zip> # exit any running Atrium instance first or omit -SmokeStart
```

## Pull requests

1. Open an issue first for anything larger than a typo, so the approach can be agreed on.
2. Keep a pull request to one topic; match the style of the surrounding code and documents.
3. Make sure `./tools/build.ps1` (native core, settings app, portable package) passes, and try the change on the running desktop.
4. Describe how you verified the change and on which Windows build.

Documents under `docs/` are currently written in Chinese; English contributions and translations are welcome.

By contributing you agree that your contributions are licensed under the [MIT License](LICENSE).
