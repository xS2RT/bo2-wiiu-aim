# Builds dist\wiiuaim.dll (32-bit). Needs a 32-bit MinGW compiler on PATH:
# llvm-mingw (i686-w64-mingw32-clang++) or mingw-w64 GCC (i686-w64-mingw32-g++).
# Or pass one: .\build.ps1 -Compiler C:\llvm-mingw\bin\i686-w64-mingw32-clang++.exe
param([string]$Compiler, [string]$Out = "$PSScriptRoot\dist")
$ErrorActionPreference = 'Stop'
if (!$Compiler) {
    $Compiler = @('i686-w64-mingw32-clang++', 'i686-w64-mingw32-g++') |
        ForEach-Object { Get-Command $_ -ErrorAction SilentlyContinue } |
        Select-Object -First 1 -ExpandProperty Source
}
if (!$Compiler) { throw 'No 32-bit MinGW compiler found. Install llvm-mingw and add its bin folder to PATH, or pass -Compiler.' }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
& $Compiler -O2 -std=c++17 -Wall "$PSScriptRoot\plugin\wiiuaim.cpp" -shared -static -s -o "$Out\wiiuaim.dll"
if ($LASTEXITCODE) { throw 'build failed' }
Write-Output "Built $Out\wiiuaim.dll"
