@echo off
setlocal
cd /d "%~dp0.."
if not exist build\compat-tests\reshade-source\source\effect_parser.hpp (
    echo ERROR: Prepare pinned ReShade sources as documented in UPSTREAM-REVIEW-2026-10-02.md.
    exit /b 2
)
if not exist build\compat-tests\ReShade.fxh (
    echo ERROR: Prepare the pinned ReShade.fxh as documented in UPSTREAM-REVIEW-2026-10-02.md.
    exit /b 2
)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %ERRORLEVEL%
cl /nologo /EHsc /O2 /MD /std:c++17 /Ibuild\compat-tests\reshade-source\source /Ibuild\compat-tests\reshade-source\deps\spirv\include\spirv\unified1 /Fobuild\compat-tests\ /Febuild\compat-tests\shader-compile.exe tests\ck3-shader-compile.cpp build\compat-tests\reshade-source\source\effect_codegen_spirv.cpp build\compat-tests\reshade-source\source\effect_expression.cpp build\compat-tests\reshade-source\source\effect_lexer.cpp build\compat-tests\reshade-source\source\effect_parser_exp.cpp build\compat-tests\reshade-source\source\effect_parser_stmt.cpp build\compat-tests\reshade-source\source\effect_preprocessor.cpp build\compat-tests\reshade-source\source\effect_symbol_table.cpp
if errorlevel 1 exit /b %ERRORLEVEL%
build\compat-tests\shader-compile.exe
exit /b %ERRORLEVEL%
