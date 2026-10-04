@echo off
REM ============================================================================
REM ReVPN.bat - native Windows wrapper for build-windows\ReVPN-engine.exe
REM
REM Same idea as the bash `ReVPN` script: with no arguments, show an
REM interactive menu; with flags, translate them straight to the engine's
REM --mode/--comm/--server arguments. No whiptail/curses here - plain
REM `set /p` prompts, since that's what a native .bat can do without any
REM extra tools, on any Windows box.
REM
REM Scope: --server, --client and --decentralized are supported.
REM --client and --decentralized need Wintun (wintun.dll from
REM https://www.wintun.net/, next to ReVPN-engine.exe - install.bat fetches
REM it for you) and Administrator; they load Wintun dynamically and refuse
REM with a clear message if the DLL is missing. There is no --stress here:
REM batch has no good way to
REM run a background server + N synthetic clients and read its output back;
REM use python\ReVPN.py --stress (or dist\ReVPN.exe --stress) for that.
REM ============================================================================
setlocal enabledelayedexpansion
set "SELF_DIR=%~dp0"
set "ENGINE=%SELF_DIR%build-windows\ReVPN-engine.exe"

if not exist "%ENGINE%" (
    echo ReVPN: engine not found at "%ENGINE%"
    echo        Build it first: build_windows.sh ^(from Linux/WSL, needs mingw-w64^)
    exit /b 1
)

set "PORT=9000"
set "WORKERS=4"
set "VPN_IP="
set "CONNECT="
set "SUBNET=16"
set "ENCRYPT=true"
set "RELAY_ONLY=false"
set "MODE="
set "DC_ID="
set "PEER_TOKENS="

if "%~1"=="" goto MENU

:PARSE
if "%~1"=="" goto AFTERPARSE
if /i "%~1"=="--server"       (set "MODE=server" & shift & goto PARSE)
if /i "%~1"=="--client"       (set "MODE=client" & shift & goto PARSE)
if /i "%~1"=="--decentralized" (set "MODE=decentralized" & shift & goto PARSE)
if /i "%~1"=="--help"       goto HELP
if /i "%~1"=="-h"           goto HELP
if /i "%~1"=="--port"       (set "PORT=%~2"     & shift & shift & goto PARSE)
if /i "%~1"=="--workers"    (set "WORKERS=%~2"  & shift & shift & goto PARSE)
if /i "%~1"=="--connect"    (set "CONNECT=%~2"  & shift & shift & goto PARSE)
if /i "%~1"=="--vpn-ip"     (set "VPN_IP=%~2"   & shift & shift & goto PARSE)
if /i "%~1"=="--subnet"     (set "SUBNET=%~2"   & shift & shift & goto PARSE)
if /i "%~1"=="--encrypt"    (set "ENCRYPT=%~2"  & shift & shift & goto PARSE)
if /i "%~1"=="--relay-only" (set "RELAY_ONLY=true" & shift & goto PARSE)
if /i "%~1"=="--id"         (set "DC_ID=%~2"    & shift & shift & goto PARSE)
if /i "%~1"=="--peer-token" (set "PEER_TOKENS=!PEER_TOKENS! --peer-token %~2" & shift & shift & goto PARSE)
echo ReVPN: unknown option '%~1' ^(see: ReVPN.bat --help^)
exit /b 1

:AFTERPARSE
if "%MODE%"=="server" goto RUNSERVER
if "%MODE%"=="client" goto RUNCLIENT
if "%MODE%"=="decentralized" goto RUNDECENTRALIZED
echo ReVPN: one of --server, --client or --decentralized is required
goto HELP

REM ============================================================================
REM Interactive menu
REM ============================================================================
:MENU
cls
echo ReVPN - Main Menu - Creative By Rezier Labs
echo ==============================================================
echo   1^) Server         Start this machine as the relay/rendezvous server
echo   2^) Client         Join a ReVPN server as a client
echo   3^) Decentralized  Connect directly to a peer - no server, just a token
echo   4^) Help           Show full --help text
echo   5^) Quit           Exit
echo.
set /p "CHOICE=Choose [1-5]: "
if "%CHOICE%"=="1" goto SERVERFORM
if "%CHOICE%"=="2" goto CLIENTFORM
if "%CHOICE%"=="3" goto DECENTRALIZEDFORM
if "%CHOICE%"=="4" goto HELP
if "%CHOICE%"=="5" exit /b 0
if "%CHOICE%"=="" exit /b 0
echo Unknown choice.
pause >nul
goto MENU

:SERVERFORM
echo.
echo -- Server Settings --
REM `set /p X=prompt` is NOT reliable about leaving X unchanged when you
REM just press Enter (Wine's cmd.exe blanks it; some real cmd.exe builds
REM keep it) - so every prompt reads into a _IN scratch var and only
REM overwrites the real one if you actually typed something.
set "PORT_IN="
set /p "PORT_IN=Port to listen on [%PORT%]: "
if not "%PORT_IN%"=="" set "PORT=%PORT_IN%"
set "WORKERS_IN="
set /p "WORKERS_IN=Worker threads [%WORKERS%]: "
if not "%WORKERS_IN%"=="" set "WORKERS=%WORKERS_IN%"
set /p "ASPEER=Also join the mesh yourself, so clients can reach this server at its own VPN IP? (y/N): "
if /i "%ASPEER%"=="y" (
    if "!VPN_IP!"=="" set "VPN_IP=10.13.0.1"
    set "VPN_IP_IN="
    set /p "VPN_IP_IN=This server's VPN IP [!VPN_IP!]: "
    if not "!VPN_IP_IN!"=="" set "VPN_IP=!VPN_IP_IN!"
    set "SUBNET_IN="
    set /p "SUBNET_IN=VPN network prefix length [!SUBNET!]: "
    if not "!SUBNET_IN!"=="" set "SUBNET=!SUBNET_IN!"
) else (
    set "VPN_IP="
)
echo.
echo Start Server:
echo   Port    : %PORT%
echo   Workers : %WORKERS%
if "%VPN_IP%"=="" (echo   VPN IP  : None ^(pure bridge^)) else (echo   VPN IP  : %VPN_IP%/%SUBNET%  ^(server joins as a peer^))
set /p "CONFIRM=Proceed? (Y/n): "
if /i "%CONFIRM%"=="n" goto MENU
goto RUNSERVER

:CLIENTFORM
echo.
echo -- Client Settings --
set "CONNECT_IN="
set /p "CONNECT_IN=Address of the server to join, ip:port [%CONNECT%]: "
if not "%CONNECT_IN%"=="" set "CONNECT=%CONNECT_IN%"
if "%VPN_IP%"=="" set "VPN_IP=10.13.0.2"
set "VPN_IP_IN="
set /p "VPN_IP_IN=This node's VPN IP [%VPN_IP%]: "
if not "%VPN_IP_IN%"=="" set "VPN_IP=%VPN_IP_IN%"
set "SUBNET_IN="
set /p "SUBNET_IN=VPN network prefix length [%SUBNET%]: "
if not "%SUBNET_IN%"=="" set "SUBNET=%SUBNET_IN%"
set /p "ENC=Encrypt tunnel traffic? (Y/n): "
if /i "%ENC%"=="n" (set "ENCRYPT=false") else (set "ENCRYPT=true")
set /p "PUNCH=Try direct UDP hole punching first, auto relay fallback? (Y/n): "
if /i "%PUNCH%"=="n" (set "RELAY_ONLY=true") else (set "RELAY_ONLY=false")
echo.
echo Join as Client:
echo   Server  : %CONNECT%
echo   VPN IP  : %VPN_IP%/%SUBNET%
echo   Encrypt : %ENCRYPT%
if "%RELAY_ONLY%"=="true" (echo   Mode    : relay-only) else (echo   Mode    : p2p + auto relay fallback)
set /p "CONFIRM=Proceed? (Y/n): "
if /i "%CONFIRM%"=="n" goto MENU
goto RUNCLIENT

:DECENTRALIZEDFORM
echo.
echo -- Decentralized Settings (no server - direct token exchange) --
if "%DC_ID%"=="" set "DC_ID=me"
set "DCID_IN="
set /p "DCID_IN=Your display name, shown in the token [%DC_ID%]: "
if not "%DCID_IN%"=="" set "DC_ID=%DCID_IN%"
if "%VPN_IP%"=="" set "VPN_IP=10.13.0.2"
set "VPN_IP_IN="
set /p "VPN_IP_IN=This node's VPN IP [%VPN_IP%]: "
if not "%VPN_IP_IN%"=="" set "VPN_IP=%VPN_IP_IN%"
set "SUBNET_IN="
set /p "SUBNET_IN=VPN network prefix length [%SUBNET%]: "
if not "%SUBNET_IN%"=="" set "SUBNET=%SUBNET_IN%"
set /p "ENC=Encrypt tunnel traffic? (Y/n): "
if /i "%ENC%"=="n" (set "ENCRYPT=false") else (set "ENCRYPT=true")
echo.
echo Connect directly to a peer - no server:
echo   Your name : %DC_ID%
echo   VPN IP    : %VPN_IP%/%SUBNET%
echo   Encrypt   : %ENCRYPT%
echo.
echo Next: this will print a short token - send that to every peer you want
echo in the mesh (chat, voice, ...) - then ask you to paste each peer's token
echo back, one per line (blank line to finish).
set /p "CONFIRM=Proceed? (Y/n): "
if /i "%CONFIRM%"=="n" goto MENU
goto RUNDECENTRALIZED

REM ============================================================================
REM Launchers
REM ============================================================================
:RUNSERVER
set "SELFPEER="
if not "%VPN_IP%"=="" set "SELFPEER=--vpn-ip %VPN_IP% --subnet %SUBNET%"
if "%VPN_IP%"=="" (
    echo ReVPN: starting server on 0.0.0.0:%PORT% ^(%WORKERS% workers^)
) else (
    echo ReVPN: starting server on 0.0.0.0:%PORT% ^(%WORKERS% workers^), joining mesh as %VPN_IP%/%SUBNET%
)
"%ENGINE%" --mode server --bind 0.0.0.0:%PORT% --workers %WORKERS% %SELFPEER%
exit /b %ERRORLEVEL%

:RUNCLIENT
if not defined VPN_IP (
    echo ReVPN: --client requires --vpn-ip ^<ip^>
    exit /b 1
)
if not defined CONNECT (
    echo ReVPN: --client requires --connect ^<ip:port^>
    exit /b 1
)
set "COMM=p2p"
if /i "%RELAY_ONLY%"=="true" set "COMM=relay"
set "ENCFLAG="
if /i "%ENCRYPT%"=="false" set "ENCFLAG=--no-encrypt"
echo ReVPN: joining %CONNECT% as %VPN_IP%/%SUBNET%  (mode=%COMM%, encrypt=%ENCRYPT%)
if /i "%COMM%"=="p2p" echo ReVPN: will try direct UDP hole punching, auto-relay on failure
"%ENGINE%" --mode client --vpn-ip %VPN_IP% --server %CONNECT% --comm %COMM% --subnet %SUBNET% %ENCFLAG%
exit /b %ERRORLEVEL%

:RUNDECENTRALIZED
if not defined DC_ID (
    echo ReVPN: --decentralized requires --id ^<name^>
    exit /b 1
)
if not defined VPN_IP (
    echo ReVPN: --decentralized requires --vpn-ip ^<ip^>
    exit /b 1
)
set "ENCFLAG="
if /i "%ENCRYPT%"=="false" set "ENCFLAG=--no-encrypt"
echo ReVPN: decentralized mode - no server, direct token exchange with your peer(s)
"%ENGINE%" --mode decentralized --id %DC_ID% --vpn-ip %VPN_IP% --subnet %SUBNET%!PEER_TOKENS! %ENCFLAG%
exit /b %ERRORLEVEL%

REM ============================================================================
:HELP
echo ReVPN.bat - native Windows wrapper for ReVPN-engine.exe
echo Creative By Rezier Labs
echo.
echo USAGE:
echo   ReVPN.bat                              Launch the interactive menu
echo   ReVPN.bat --server  [options]          Start this machine as the relay/rendezvous server
echo   ReVPN.bat --client  [options]          Join a ReVPN server as a client
echo   ReVPN.bat --decentralized [options]    Join a peer directly, no server - just a token
echo   ReVPN.bat --help, -h                   Show this help
echo.
echo SERVER OPTIONS:
echo   --port ^<n^>          UDP port to listen on         (default: 9000)
echo   --workers ^<n^>       Server worker threads          (default: 4)
echo   --vpn-ip ^<ip^>       Also join the mesh as a peer, reachable
echo                       at this VPN IP (default: pure bridge, no TUN)
echo   --subnet ^<n^>        VPN network prefix length, with --vpn-ip (default: 16)
echo.
echo CLIENT OPTIONS:
echo   --connect ^<ip:port^>  Address of the ReVPN server to join    (required)
echo   --vpn-ip ^<ip^>        This node's VPN IP, e.g. 10.13.0.2      (required)
echo   --subnet ^<n^>         VPN network prefix length              (default: 16)
echo   --encrypt ^<true^|false^>  Encrypt tunnel traffic             (default: true)
echo   --relay-only          Never attempt direct UDP hole punching
echo.
echo DECENTRALIZED OPTIONS:
echo   --id ^<name^>          Your display name in the token          (required)
echo   --vpn-ip ^<ip^>        This node's VPN IP, e.g. 10.13.0.2       (required)
echo   --subnet ^<n^>         VPN network prefix length               (default: 16)
echo   --peer-token ^<tok^>   A peer's token - repeat this flag once per
echo                        peer for a mesh of 3+. Skips the paste prompt.
echo   --encrypt ^<true^|false^>  Encrypt tunnel traffic              (default: true)
echo.
echo   No rendezvous/relay server anywhere - each side asks a public STUN
echo   server for its own address, packs it into a short token, and everyone
echo   in the mesh exchanges tokens directly (chat, voice, however). With
echo   no --peer-token, it prints your token, then waits for you to paste
echo   one peer's token per line (blank line to finish).
echo.
echo NOTE: --client and --decentralized need Wintun - download wintun.dll
echo       from https://www.wintun.net/ and place it next to
echo       ReVPN-engine.exe (install.bat does this for you), then run this
echo       as Administrator. Untested on real Windows hardware so far
echo       (built/run only under Wine, which has no Wintun driver to
echo       actually exercise it against).
echo.
echo NOTE: no --stress here - use python\ReVPN.py --stress instead.
exit /b 0
