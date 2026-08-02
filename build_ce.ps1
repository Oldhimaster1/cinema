$ErrorActionPreference = 'Stop'
if (-not (Get-Command cedev-config -ErrorAction SilentlyContinue)) {
    throw 'CEdev Toolchain is not installed or cedev-config is not on PATH.'
}
make clean
make -j $env:NUMBER_OF_PROCESSORS
if (-not (Test-Path 'bin/CINEMA.8xp')) { throw 'Build completed without bin/CINEMA.8xp' }
Get-Item 'bin/CINEMA.8xp' | Format-List Name,Length,FullName
Get-FileHash 'bin/CINEMA.8xp' -Algorithm SHA256
