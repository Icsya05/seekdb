param(
  [Parameter(Mandatory=$true)][string]$BuildDir,
  [string]$Cc='gcc',
  [string]$Cxx='g++'
)
$ErrorActionPreference='Stop'
$BuildDir=[IO.Path]::GetFullPath($BuildDir)
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$bridge=Join-Path $PSScriptRoot '../seekdb_cuvs_bridge.c'
$test=Join-Path $PSScriptRoot 'test_search_workspace.cpp'
$include=Join-Path $PSScriptRoot 'include'
$object=Join-Path $BuildDir 'bridge-c.o'
$exeC=Join-Path $BuildDir 'test_search_workspace_c.exe'
$exeCpp=Join-Path $BuildDir 'test_search_workspace_cpp.exe'
& $Cc -std=c11 -Wall -Wextra -Werror -pthread -I $include -c $bridge -o $object
if ($LASTEXITCODE -ne 0) { throw 'C bridge compilation failed' }
& $Cxx -std=c++17 -Wall -Wextra -Werror -pthread -I $include $object $test -o $exeC
if ($LASTEXITCODE -ne 0) { throw 'C host test link failed' }
& $exeC
if ($LASTEXITCODE -ne 0) { throw 'C host tests failed' }
& $Cxx -std=c++17 -Wall -Wextra -Werror -pthread -x c++ -I $include $bridge $test -o $exeCpp
if ($LASTEXITCODE -ne 0) { throw 'C++ bridge compilation failed' }
& $exeCpp
if ($LASTEXITCODE -ne 0) { throw 'C++ host tests failed' }
