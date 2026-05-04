@echo off
REM build.bat — сборка TON-Tunnel из Git Bash / cmd
REM Запускать: cmd /c build.bat  (из Git Bash)
REM или просто: build.bat  (из cmd)

cd /d "%~dp0"

REM Удалить старую сборку
if exist build-backend-msvc rmdir /s /q build-backend-msvc

REM Найти vcvars64.bat
set "VSWHERE=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VSWHERE%" (
    echo ERROR: vcvars64.bat not found at %VSWHERE%
    echo Install Visual Studio 2022 with C++ workload
    exit /b 1
)

REM Инициализировать MSVC окружение
call "%VSWHERE%"

REM Добавить Strawberry Perl в PATH
set PATH=C:\Strawberry\perl\bin;%PATH%

REM Генерация CMake
cmake -B build-backend-msvc -G "Visual Studio 17 2022" -A x64 ^
    -DPERL_EXECUTABLE=C:/Strawberry/perl/bin/perl.exe ^
    -DPKG_CONFIG_EXECUTABLE=C:/Strawberry/perl/bin/pkg-config.bat

if %ERRORLEVEL% neq 0 (
    echo CMake configuration failed!
    exit /b %ERRORLEVEL%
)

echo.
echo ===== CMake configuration OK =====
echo.

REM Сборка
cmake --build build-backend-msvc --config Release

if %ERRORLEVEL% neq 0 (
    echo Build failed!
    exit /b %ERRORLEVEL%
)

echo.
echo ===== BUILD SUCCESS =====
echo Output: build-backend-msvc\Release\TON_Tunnel.exe
pause
