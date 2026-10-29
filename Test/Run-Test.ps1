# 테스트 시나리오 실행기
#   .\Test\Run-Test.ps1                         사용 가능한 시나리오 목록
#   .\Test\Run-Test.ps1 -Scenario EnterLeave    시나리오 하나 (여러 개면 쉼표로)
#   .\Test\Run-Test.ps1 -All -Build             빌드 후 전체 실행
#   스크립트 실행이 막혀 있으면: powershell -ExecutionPolicy Bypass -File .\Test\Run-Test.ps1 -All
param(
    [string[]]$Scenario,
    [switch]$All,
    [switch]$Build,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'TestLib.psm1') -Force

# _로 시작하는 파일(_Template.ps1)은 시나리오 목록에서 제외
$scenarioDir = Join-Path $PSScriptRoot 'Scenarios'
$available = @(Get-ChildItem $scenarioDir -Filter '*.ps1' | Where-Object { -not $_.Name.StartsWith('_') } | ForEach-Object { $_.BaseName })

if (-not $All -and -not $Scenario)
{
    Write-Host 'Usage: .\Test\Run-Test.ps1 -Scenario <name>[,<name>] | -All  [-Build] [-Configuration Debug|Release]'
    Write-Host "Scenarios: $($available -join ', ')"
    exit 1
}

$targets = if ($All) { $available } else { $Scenario }

if ($Build) { Invoke-SolutionBuild -Configuration $Configuration }

$runDir = Join-Path $PSScriptRoot ('logs\' + (Get-Date -Format 'yyyyMMdd_HHmmss'))
$results = @()

foreach ($name in $targets)
{
    $file = Join-Path $scenarioDir "$name.ps1"
    Write-Host ''
    Write-Host "===== $name =====" -ForegroundColor Cyan

    if (-not (Test-Path $file))
    {
        Write-Host "  [x] scenario not found: $file" -ForegroundColor Red
        $results += [pscustomobject]@{ Name = $name; Passed = $false }
        continue
    }

    Initialize-TestContext -Configuration $Configuration -LogDir (Join-Path $runDir $name)
    try
    {
        . $file
    }
    catch
    {
        Add-TestFailure "aborted: $($_.Exception.Message)"
    }
    finally
    {
        Stop-AllTestProcesses   # 실패해도 띄운 서버/클라이언트는 반드시 정리
    }

    $failures = Get-TestFailures
    $results += [pscustomobject]@{ Name = $name; Passed = ($failures.Count -eq 0) }
}

Write-Host ''
Write-Host '===== Result =====' -ForegroundColor Cyan
foreach ($result in $results)
{
    if ($result.Passed) { Write-Host "[PASS] $($result.Name)" -ForegroundColor Green }
    else { Write-Host "[FAIL] $($result.Name)" -ForegroundColor Red }
}
Write-Host "Logs: $runDir"

if (@($results | Where-Object { -not $_.Passed }).Count -gt 0) { exit 1 }
exit 0
