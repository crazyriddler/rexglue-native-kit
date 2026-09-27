@echo off
setlocal
cd /d "%~dp0"

where claude >nul 2>nul
if errorlevel 1 (
  echo [ERROR] Claude Code is not available in PATH.
  echo Install or update Claude Code and run this file again.
  pause
  exit /b 1
)

echo ============================================================
echo  ReXGlue Native Port Kit - Claude Code
echo  Folder: %CD%
echo  GitHub remote: NOT required
echo  Mode: bypassPermissions
echo ============================================================
echo.

powershell -NoProfile -ExecutionPolicy Bypass -Command "$p = Get-Content -Raw -LiteralPath '.\START_PROMPT.md'; & claude --permission-mode bypassPermissions $p"

endlocal
