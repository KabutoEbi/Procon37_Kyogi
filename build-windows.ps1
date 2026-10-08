[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$Generator = "",
    [switch]$Test
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Build = Join-Path $Root ".build/windows"

$Configure = @("-S", $Root, "-B", $Build, "-DCMAKE_BUILD_TYPE=$Configuration")
if ($Generator) {
    $Configure += @("-G", $Generator)
}

& cmake @Configure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& cmake --build $Build --config $Configuration --target procon_solver
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Test) {
    & cmake --build $Build --config $Configuration
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & ctest --test-dir $Build -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "Built: $(Join-Path $Root 'main.exe')"
