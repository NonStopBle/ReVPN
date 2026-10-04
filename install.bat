@echo off
REM ============================================================================
REM install.bat - installs ReVPN's Windows dependencies.
REM
REM What it does:
REM   1. Checks build-windows\ReVPN-engine.exe exists (tells you how to
REM      build it if not - requires Linux/WSL + mingw-w64, see build_windows.sh).
REM   2. Downloads wintun.dll (needed by --client and --decentralized) from
REM      https://www.wintun.net/ and drops the right architecture copy next
REM      to ReVPN-engine.exe.
REM   3. Checks for Python 3 (optional - only needed for python\ReVPN.py).
REM
REM Run this once after cloning/extracting the repo on a Windows machine,
REM before using ReVPN.bat --client or ReVPN.bat --decentralized.
REM ============================================================================
setlocal enabledelayedexpansion
set "SELF_DIR=%~dp0"
set "ENGINE_DIR=%SELF_DIR%build-windows"
set "ENGINE=%ENGINE_DIR%\ReVPN-engine.exe"
set "WINTUN_DLL=%ENGINE_DIR%\wintun.dll"
set "WINTUN_VERSION=0.14.1"
set "WINTUN_URL=https://www.wintun.net/builds/wintun-%WINTUN_VERSION%.zip"
set "TMP_ZIP=%TEMP%\revpn-wintun-%RANDOM%.zip"
set "TMP_EXTRACT=%TEMP%\revpn-wintun-%RANDOM%"

echo ReVPN install - Windows dependency setup
echo ==============================================================
echo.

REM --- 1. Engine binary -------------------------------------------------
if exist "%ENGINE%" (
    echo [OK] Engine found: %ENGINE%
) else (
    echo [MISSING] Engine not found at %ENGINE%
    echo           Build it from Linux/WSL with mingw-w64:
    echo             sudo apt install g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64
    echo             ./build_windows.sh
    echo           Then re-run this script.
)
echo.

REM --- 2. Architecture detect --------------------------------------------
set "ARCH=amd64"
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "ARCH=arm64"
if defined PROCESSOR_ARCHITEW6432 if /i "%PROCESSOR_ARCHITEW6432%"=="ARM64" set "ARCH=arm64"
if /i "%PROCESSOR_ARCHITECTURE%"=="x86" if not defined PROCESSOR_ARCHITEW6432 set "ARCH=x86"

echo [INFO] Detected architecture: %ARCH%
echo.

REM --- 3. Wintun driver DLL ----------------------------------------------
if exist "%WINTUN_DLL%" (
    echo [OK] wintun.dll already present: %WINTUN_DLL%
    goto PYTHON_CHECK
)

if not exist "%ENGINE_DIR%" mkdir "%ENGINE_DIR%"

echo [INFO] Downloading Wintun %WINTUN_VERSION% from %WINTUN_URL% ...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri '%WINTUN_URL%' -OutFile '%TMP_ZIP%' -UseBasicParsing } catch { exit 1 }"
if errorlevel 1 (
    echo [FAIL] Could not download wintun.dll automatically.
    echo        Download it yourself from https://www.wintun.net/
    echo        and copy wintun\bin\%ARCH%\wintun.dll to:
    echo          %WINTUN_DLL%
    goto PYTHON_CHECK
)

echo [INFO] Extracting ...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "try { Expand-Archive -Path '%TMP_ZIP%' -DestinationPath '%TMP_EXTRACT%' -Force } catch { exit 1 }"
if errorlevel 1 (
    echo [FAIL] Could not extract the Wintun archive.
    del /q "%TMP_ZIP%" >nul 2>&1
    goto PYTHON_CHECK
)

set "SRC_DLL=%TMP_EXTRACT%\wintun\bin\%ARCH%\wintun.dll"
if not exist "%SRC_DLL%" set "SRC_DLL=%TMP_EXTRACT%\wintun\bin\amd64\wintun.dll"

if exist "%SRC_DLL%" (
    copy /y "%SRC_DLL%" "%WINTUN_DLL%" >nul
    echo [OK] Installed: %WINTUN_DLL%
) else (
    echo [FAIL] Expected DLL not found in downloaded archive.
    echo        Download it yourself from https://www.wintun.net/
    echo        and copy wintun\bin\%ARCH%\wintun.dll to:
    echo          %WINTUN_DLL%
)

del /q "%TMP_ZIP%" >nul 2>&1
rmdir /s /q "%TMP_EXTRACT%" >nul 2>&1

:PYTHON_CHECK
echo.
where python >nul 2>&1
if errorlevel 1 (
    where python3 >nul 2>&1
    if errorlevel 1 (
        echo [INFO] Python not found on PATH - only needed for python\ReVPN.py
        echo        ^(optional: the ReVPN.bat + ReVPN-engine.exe path above
        echo        does not need Python at all^). Get it from
        echo        https://www.python.org/downloads/ if you want it.
    ) else (
        echo [OK] python3 found on PATH.
    )
) else (
    echo [OK] python found on PATH.
)

echo.
echo ==============================================================
echo Done. Remember:
echo   - ReVPN.bat --client and --decentralized need Administrator
echo     privileges to create the Wintun network adapter.
echo   - Run ReVPN.bat with no arguments for the interactive menu,
echo     or ReVPN.bat --help for all flags.
echo ==============================================================
exit /b 0
