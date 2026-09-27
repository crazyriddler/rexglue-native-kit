@echo off
setlocal
cd /d "%~dp0"

where claude >nul 2>nul
if errorlevel 1 (
  echo [ERROR] Claude Code no esta disponible en PATH.
  echo Instala/actualiza Claude Code y vuelve a ejecutar este archivo.
  pause
  exit /b 1
)

echo ============================================================
echo  ReXGlue Native Port Kit - Claude Code
echo  Carpeta: %CD%
echo  GitHub remoto: NO requerido
echo  Modo: bypassPermissions
echo ============================================================
echo.

powershell -NoProfile -ExecutionPolicy Bypass -Command "$p = Get-Content -Raw -LiteralPath '.\START_PROMPT.md'; & claude --permission-mode bypassPermissions $p"

endlocal
