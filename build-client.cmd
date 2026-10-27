@echo off
setlocal
rem Run from an x64 Visual Studio Developer Command Prompt with clang-cl on PATH.
where clang-cl >nul 2>&1 || (echo clang-cl not found. Install LLVM and use an x64 Developer Command Prompt. & exit /b 1)
if not defined VCToolsInstallDir (echo Visual Studio x64 build environment is required. & exit /b 1)
set "SRC=%~dp0emulator\mod"
set "OUT=%~dp0build\bbsc-mod"
set "OBJ=%OUT%\obj"
if not exist "%OBJ%" mkdir "%OBJ%"
rem Public binaries must not embed the developer's absolute workspace path in diagnostics.
set "PUBLIC_PATH_FLAG=/clang:-ffile-prefix-map="%~dp0=./""
set "EXT=%SRC%\..\externals"
set "FLAGS=/nologo /std:c++latest /O2 /MT /EHsc /W3 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DBBSC_MOD /DFMT_HEADER_ONLY /I "%SRC%\shim" /I "%SRC%\..\src" /I "%EXT%\fmt\include" /I "%EXT%\xbyak""
clang-cl %FLAGS% %PUBLIC_PATH_FLAG% /LD "%SRC%\bbsc_mod.cpp" "%SRC%\bbsc_hooks.cpp" "%SRC%\bbsc_relay.cpp" "%SRC%\bbsc_channel.cpp" "%SRC%\bbsc_multiplayer.cpp" "%SRC%\bbsc_http.cpp" "%SRC%\bbsc_discord.cpp" "%SRC%\..\src\core\bbsc_memscan.cpp" "%SRC%\..\src\core\bloodborne_re.cpp" /Fo"%OBJ%\\" /Fe"%OUT%\bbsc.dll" /link ws2_32.lib winhttp.lib user32.lib || exit /b 1
clang-cl %FLAGS% %PUBLIC_PATH_FLAG% "%SRC%\bbsc_launch.cpp" /Fo"%OBJ%\\" /Fe"%OUT%\bbsc-launch.exe" /link user32.lib comdlg32.lib /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup || exit /b 1
clang-cl /nologo /std:c++17 /EHsc "%SRC%\tests\multiplayer_readiness.cpp" /Fo"%OBJ%\\" /Fe"%OUT%\readiness-test.exe" || exit /b 1
"%OUT%\readiness-test.exe" || exit /b 1
clang-cl /nologo /std:c++20 /EHsc "%SRC%\tests\respawn_regressions.cpp" /Fo"%OBJ%\\" /Fe"%OUT%\respawn-test.exe" || exit /b 1
"%OUT%\respawn-test.exe" || exit /b 1
clang-cl %FLAGS% "%SRC%\tests\early_hook_reservation.cpp" "%SRC%\bbsc_hooks.cpp" /Fo"%OBJ%\\" /Fe"%OUT%\hook-reservation-test.exe" || exit /b 1
"%OUT%\hook-reservation-test.exe" || exit /b 1
echo Build complete: %OUT%
