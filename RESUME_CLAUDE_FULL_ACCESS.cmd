@echo off
setlocal
cd /d "%~dp0"

where claude >nul 2>nul
if errorlevel 1 (
  echo [ERROR] Claude Code no esta disponible en PATH.
  pause
  exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -Command "$p = Get-Content -Raw -LiteralPath '.\RESUME_PROMPT.md'; & claude --permission-mode bypassPermissions $p"
endlocal
