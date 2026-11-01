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

# Test the actual smoke program's write-mode control flow against the CPU API boundary.
$smoke=Join-Path $BuildDir 'test_persistence_host.exe'
& $Cxx -std=c++17 -Wall -Wextra -Werror -pthread -I $include $object (Join-Path $PSScriptRoot 'test_persistence_host.cpp') -o $smoke
if ($LASTEXITCODE -ne 0) { throw 'Host persistence-smoke compilation failed' }
& $smoke write host-fixture
if ($LASTEXITCODE -ne 0) { throw 'Smoke success path failed' }
& $smoke write host-fixture serialize-failure
if ($LASTEXITCODE -ne 3) { throw 'Serialization failure must release the handle and return 3' }
& $smoke write host-fixture copy-failure
if ($LASTEXITCODE -ne 5) { throw 'Copy failure must release the handle and return 5' }
& $smoke typo host-fixture
if ($LASTEXITCODE -ne 2) { throw 'Invalid mode must return 2' }
Write-Output 'PASS smoke host control-flow and error cleanup (no GPU persistence claim)'
exit 0
