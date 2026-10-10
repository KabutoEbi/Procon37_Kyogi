[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$Generator = "",
    [switch]$Test,
    [switch]$Benchmark
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Build = Join-Path $Root ".build/windows"

$BuildTests = if ($Test) { "ON" } else { "OFF" }
$BuildBenchmark = if ($Benchmark) { "ON" } else { "OFF" }
$Configure = @(
    "-S", $Root,
    "-B", $Build,
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DPROCON_BUILD_TESTS=$BuildTests",
    "-DPROCON_BUILD_BENCHMARK=$BuildBenchmark"
)
if ($Generator) {
    $Configure += @("-G", $Generator)
}

& cmake @Configure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& cmake --build $Build --config $Configuration
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Test) {
    & ctest --test-dir $Build -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "Built: $(Join-Path $Root 'main.exe')"
if ($Benchmark) {
    Write-Host "Built: $(Join-Path $Build 'tools/quantity_joint_benchmark.exe')"
}
