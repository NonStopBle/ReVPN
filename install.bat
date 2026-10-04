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
REM   3. Adds a Windows Firewall rule allowing inbound UDP to
REM      ReVPN-engine.exe. Without this, Windows Firewall silently drops
REM      unsolicited inbound UDP packets (server traffic, and - critically
REM      for --decentralized/--client P2P - the other peer's hole-punch
REM      and ACK packets), which looks like "TX keeps climbing, RX stays
REM      at 0 forever" even though both sides are sending correctly.
REM   4. Checks for Python 3 (optional - only needed for python\ReVPN.py).
REM
REM Run this once after cloning/extracting the repo on a Windows machine,
REM as Administrator, before using ReVPN.bat --client or --decentralized.
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
    goto FIREWALL_CHECK
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
    goto FIREWALL_CHECK
)

echo [INFO] Extracting ...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "try { Expand-Archive -Path '%TMP_ZIP%' -DestinationPath '%TMP_EXTRACT%' -Force } catch { exit 1 }"
if errorlevel 1 (
    echo [FAIL] Could not extract the Wintun archive.
    del /q "%TMP_ZIP%" >nul 2>&1
    goto FIREWALL_CHECK
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

:FIREWALL_CHECK
echo.
net session >nul 2>&1
if errorlevel 1 (
    echo [SKIP] Not running as Administrator - cannot add a firewall rule.
    echo        Re-run install.bat as Administrator, or allow
    echo        "%ENGINE%" through Windows Firewall yourself
    echo        ^(inbound AND outbound, UDP^) when prompted.
    goto PYTHON_CHECK
)

if not exist "%ENGINE%" (
    echo [SKIP] Engine not built yet - run install.bat again after building
    echo        it to add the firewall rule.
    goto PYTHON_CHECK
)

netsh advfirewall firewall show rule name="ReVPN-engine" >nul 2>&1
if not errorlevel 1 (
    echo [OK] Firewall rule "ReVPN-engine" already present.
    goto PYTHON_CHECK
)

echo [INFO] Adding Windows Firewall rules for ReVPN-engine.exe ^(inbound + outbound UDP^) ...
netsh advfirewall firewall add rule name="ReVPN-engine" dir=in action=allow program="%ENGINE%" protocol=UDP enable=yes >nul
netsh advfirewall firewall add rule name="ReVPN-engine" dir=out action=allow program="%ENGINE%" protocol=UDP enable=yes >nul
if errorlevel 1 (
    echo [FAIL] Could not add the firewall rule automatically.
    echo        Without it, Windows Firewall silently drops unsolicited
    echo        inbound UDP ^(server traffic, and peer hole-punch/ACK
    echo        packets in --client/--decentralized modes^) - this looks
    echo        like a connection that sends fine but never receives.
    echo        Add it yourself: Windows Defender Firewall -^> Advanced
    echo        Settings -^> Inbound Rules -^> New Rule -^> Program -^>
    echo        %ENGINE%  -^> Allow, for UDP.
) else (
    echo [OK] Firewall rule added.
)

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
