# 테스트 시나리오에서 쓰는 공통 함수 모음
#   Run-Test.ps1이 Import-Module로 불러온다. 시나리오 파일(Scenarios/*.ps1)은 이 함수들만 호출하면 된다.

$script:RepoRoot  = Split-Path $PSScriptRoot -Parent
$script:Config    = 'Debug'
$script:LogDir    = $null
$script:Processes = [ordered]@{}   # 이름 -> Process ('server', 'A', 'B' ...)
$script:Failures  = New-Object System.Collections.Generic.List[string]

# ==================== 실행 준비 (Run-Test.ps1에서 호출) ====================

function Initialize-TestContext
{
    param([string]$Configuration, [string]$LogDir)

    $script:Config    = $Configuration
    $script:LogDir    = $LogDir
    $script:Processes = [ordered]@{}
    $script:Failures  = New-Object System.Collections.Generic.List[string]
    New-Item -ItemType Directory -Force $LogDir | Out-Null
}

function Get-TestFailures { return ,$script:Failures.ToArray() }

function Add-TestFailure
{
    param([string]$Message)
    $script:Failures.Add($Message)
    Write-Host "  [x] $Message" -ForegroundColor Red
}

function Get-BinDir { return Join-Path $script:RepoRoot "Binary\$script:Config" }

function Invoke-SolutionBuild
{
    param([string]$Configuration)

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $msbuild = & $vswhere -latest -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (-not $msbuild) { throw 'MSBuild.exe not found (Visual Studio 2022 required)' }

    Write-Host "Building MMO_Server.sln ($Configuration|x64) ..."
    & $msbuild (Join-Path $script:RepoRoot 'MMO_Server.sln') "/p:Configuration=$Configuration" '/p:Platform=x64' '/m' '/v:minimal' '/nologo' | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Build failed (exit code $LASTEXITCODE)" }
}

# ==================== 프로세스 ====================

function Start-TestProcess
{
    param([string]$Name, [string]$Exe, [string]$Arguments)

    if ($script:Processes.Contains($Name)) { throw "Process name '$Name' is already used in this scenario" }

    $exePath = Join-Path (Get-BinDir) $Exe
    if (-not (Test-Path $exePath)) { throw "$Exe not found: $exePath (build first, or run with -Build)" }

    $startArgs = @{
        FilePath               = $exePath
        WorkingDirectory       = Get-BinDir
        NoNewWindow            = $true
        PassThru               = $true
        RedirectStandardOutput = Join-Path $script:LogDir "$Name.txt"
        RedirectStandardError  = Join-Path $script:LogDir "$Name.err.txt"
    }
    if ($Arguments) { $startArgs.ArgumentList = $Arguments }

    $process = Start-Process @startArgs
    $null = $process.Handle   # 핸들을 잡아 둬야 나중에 ExitCode를 읽을 수 있음
    $script:Processes[$Name] = $process
    return $process
}

# 서버 실행 후 "listening" 로그가 뜰 때까지 대기. 실패하면 시나리오 중단(throw)
function Start-GameServer
{
    param([int]$TimeoutSec = 10)

    $process = Start-TestProcess -Name 'server' -Exe 'GameServer.exe'
    $deadline = (Get-Date).AddSeconds($TimeoutSec)

    while ((Get-Date) -lt $deadline)
    {
        if ((Get-LogMatchCount -Name 'server' -Pattern 'listening') -gt 0)
        {
            Write-Host '  server started'
            return
        }
        if ($process.HasExited)
        {
            $tail = (Get-Content (Join-Path $script:LogDir 'server.txt') -ErrorAction SilentlyContinue | Select-Object -Last 3) -join ' / '
            throw "GameServer exited during startup (exit $($process.ExitCode)): $tail"
        }
        Start-Sleep -Milliseconds 100
    }
    throw "GameServer did not start within $TimeoutSec s"
}

# 이름 붙인 DummyClient 그룹 실행 (프로세스 하나에 Count명). 추가 옵션은 -Arguments로 전달
function Start-DummyClients
{
    param([string]$Name, [int]$Count = 1, [string]$Arguments = '')

    $null = Start-TestProcess -Name $Name -Exe 'DummyClient.exe' -Arguments "--count $Count $Arguments".Trim()
    Write-Host "  clients '$Name' started (count $Count)"
}

# 그룹 강제 종료 = 해당 클라이언트들의 연결이 한꺼번에 끊기는 상황
function Stop-DummyClients
{
    param([string]$Name)

    if (-not $script:Processes.Contains($Name)) { throw "Unknown client group '$Name'" }
    $process = $script:Processes[$Name]
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    Write-Host "  clients '$Name' stopped"
}

function Stop-AllTestProcesses
{
    foreach ($process in $script:Processes.Values)
    {
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
    }
}

function Wait-Seconds
{
    param([double]$Seconds)
    Start-Sleep -Milliseconds ([int]($Seconds * 1000))
}

# ==================== 로그 검사 ====================

# 로그 파일에서 정규식 패턴이 나온 줄 수
function Get-LogMatchCount
{
    param([string]$Name, [string]$Pattern)

    $path = Join-Path $script:LogDir "$Name.txt"
    if (-not (Test-Path $path)) { return 0 }
    return @(Select-String -Path $path -Pattern $Pattern).Count
}

# 패턴이 Expected 줄 이상 나올 때까지 기다림 (고정 대기보다 빠르고 덜 흔들림). 시간 초과는 실패로 기록
function Wait-LogCount
{
    param([string]$Name, [string]$Pattern, [int]$Expected, [int]$TimeoutSec = 10)

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline)
    {
        if ((Get-LogMatchCount -Name $Name -Pattern $Pattern) -ge $Expected) { return }
        Start-Sleep -Milliseconds 100
    }
    $actual = Get-LogMatchCount -Name $Name -Pattern $Pattern
    Add-TestFailure "[$Name] timed out waiting for '$Pattern': $actual / $Expected lines"
}

function Assert-LogCount
{
    param([string]$Name, [string]$Pattern, [int]$Expected)

    $actual = Get-LogMatchCount -Name $Name -Pattern $Pattern
    if ($actual -eq $Expected) { Write-Host "  [ok] [$Name] '$Pattern' x $actual" -ForegroundColor Green }
    else { Add-TestFailure "[$Name] '$Pattern' x $actual (expected $Expected)" }
}

function Assert-LogContains
{
    param([string]$Name, [string]$Pattern)

    $actual = Get-LogMatchCount -Name $Name -Pattern $Pattern
    if ($actual -gt 0) { Write-Host "  [ok] [$Name] contains '$Pattern'" -ForegroundColor Green }
    else { Add-TestFailure "[$Name] does not contain '$Pattern'" }
}

function Assert-LogNotContains
{
    param([string]$Name, [string]$Pattern)

    $actual = Get-LogMatchCount -Name $Name -Pattern $Pattern
    if ($actual -eq 0) { Write-Host "  [ok] [$Name] has no '$Pattern'" -ForegroundColor Green }
    else { Add-TestFailure "[$Name] contains '$Pattern' x $actual" }
}

function Assert-ServerAlive
{
    $process = $script:Processes['server']
    if ($process -and -not $process.HasExited) { Write-Host '  [ok] server alive' -ForegroundColor Green }
    else { Add-TestFailure 'server is not running (crashed or exited)' }
}

Export-ModuleMember -Function *
