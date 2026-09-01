@echo off
rem ============================================================
rem  ChatHub one-click launcher
rem  Starts auth-service (HTTP:3000) and chat-server (TCP:9000)
rem  Usage: double-click, or run: start_all.bat
rem  Note: keep this file ASCII-only. Chinese text in a .bat
rem  breaks under the default GBK console codepage.
rem  Note: use FULL paths for exe - "cd /d X && exe" fails in
rem  batch mode with "not recognized" (cmd does not always
rem  resolve the exe from the cd'ed directory).
rem ============================================================

set PROJECT_ROOT=D:\CppLearn\chathub

echo ============================================
echo   ChatHub launcher
echo   1. auth-service  -^> http://localhost:3000
echo   2. chat-server   -^> tcp://localhost:9000
echo ============================================
echo.

echo Starting auth-service ...
start "auth-service" cmd /k "cd /d %PROJECT_ROOT%\auth-service && npm start"

echo Starting chat-server ...
start "chat-server" cmd /k "%PROJECT_ROOT%\build\chat-server\chat-server.exe"

echo.
echo Both services started. Wait a moment for them to be ready.
echo Close each window to stop the corresponding service.
echo.
