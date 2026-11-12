# 테스트 시나리오에서 쓰는 공통 함수 모음
#   Run-Test.ps1이 Import-Module로 불러온다. 시나리오 파일(Scenarios/*.ps1)은 이 함수들만 호출하면 된다.

$script:RepoRoot  = Split-Path $PSScriptRoot -Parent
$script:Config    = 'Debug'
$script:LogDir    = $null
$script:Processes = [ordered]@{}   # 이름 -> Process ('server', 'A', 'B' ...)
$script:Failures  = New-Object System.Collections.Generic.List[string]
$script:GameDataOverridden = $false   # 이 시나리오가 Set-GameData로 기획 데이터를 바꿨는지 (끝나면 Run-Test가 복구)
$script:RunStamp  = ''                # 시나리오마다 새로 정하는 시각 (DummyClient 계정 이름에 붙여서 시나리오끼리 캐릭터를 안 나눠 씀)

# ==================== 실행 준비 (Run-Test.ps1에서 호출) ====================

function Initialize-TestContext
{
    param([string]$Configuration, [string]$LogDir)

    $script:Config    = $Configuration
    $script:LogDir    = $LogDir
    $script:Processes = [ordered]@{}
    $script:Failures  = New-Object System.Collections.Generic.List[string]
    $script:GameDataOverridden = $false
    $script:RunStamp  = Get-Date -Format 'MMddHHmmss'
    New-Item -ItemType Directory -Force $LogDir | Out-Null
}

function Test-GameDataOverridden { return $script:GameDataOverridden }

# 시나리오 파일에서 쓰는 이번 시나리오의 시각 (모듈 변수라 시나리오에서 $script:로는 안 보임)
function Get-RunStamp { return $script:RunStamp }

# ==================== 기획 데이터 ====================

# 이 시나리오에서만 기획 데이터 수치를 덮어씀 (예: 몬스터 데미지를 올려서 빨리 죽게)
#   서버는 시작할 때 한 번만 읽으므로 Start-GameServer 전에 호출
#   Data/*.sql을 다시 적용한 뒤 덮어쓰고, 시나리오가 끝나면(실패해도) Run-Test가 Setup으로 원래 값을 되돌림
function Set-GameData
{
    param([string[]]$Sql)

    & (Join-Path $script:RepoRoot 'Database\Setup-Database.ps1') -ExtraSql $Sql | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Set-GameData failed: $($Sql -join '; ')" }

    $script:GameDataOverridden = $true
    Write-Host "  game data overridden: $($Sql -join '; ')"
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

    # 아웃게임 서버(.NET)는 C++ 솔루션과 따로 빌드
    Write-Host "Building OutGameServer ($Configuration) ..."
    & dotnet build (Join-Path $script:RepoRoot 'OutGameServer') -c $Configuration -nologo -v q | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "OutGameServer build failed (exit code $LASTEXITCODE)" }
}

# ==================== 프로세스 ====================

# -Exe: Binary/<구성>/ 안의 실행 파일 이름 (GameServer, DummyClient)
# -ExePath: 그 밖의 실행 파일 전체 경로 (OutGameServer). 작업 폴더는 실행 파일이 있는 폴더 (설정 파일을 거기서 읽음)
function Start-TestProcess
{
    param([string]$Name, [string]$Exe, [string]$Arguments, [string]$ExePath)

    if ($script:Processes.Contains($Name)) { throw "Process name '$Name' is already used in this scenario" }

    if (-not $ExePath) { $ExePath = Join-Path (Get-BinDir) $Exe }
    if (-not (Test-Path $ExePath)) { throw "$(Split-Path $ExePath -Leaf) not found: $ExePath (build first, or run with -Build)" }

    $startArgs = @{
        FilePath               = $ExePath
        WorkingDirectory       = Split-Path $ExePath -Parent
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
#   DummyClient가 아웃게임 서버에서 로그인한 토큰으로 입장하므로, 아웃게임 서버가 아직 없으면 먼저 띄움
function Start-GameServer
{
    param([int]$TimeoutSec = 10)

    if (-not $script:Processes.Contains('outgame')) { Start-OutGameServer }

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

# 아웃게임 서버(.NET) 실행 후 "Now listening on" 로그가 뜰 때까지 대기. 로그 이름은 'outgame'
#   비밀값(DB 연결, JWT 키)은 user-secrets에서 읽음 -> 처음 한 번 OutGameServer/Setup-Secrets.ps1 필요
function Start-OutGameServer
{
    param([int]$TimeoutSec = 20)

    $exePath = Join-Path $script:RepoRoot "OutGameServer\bin\$script:Config\net10.0\OutGameServer.exe"
    $process = Start-TestProcess -Name 'outgame' -ExePath $exePath
    $deadline = (Get-Date).AddSeconds($TimeoutSec)

    while ((Get-Date) -lt $deadline)
    {
        if ((Get-LogMatchCount -Name 'outgame' -Pattern 'Now listening on') -gt 0)
        {
            Write-Host '  outgame server started'
            return
        }
        if ($process.HasExited)
        {
            $log = @(Get-Content (Join-Path $script:LogDir 'outgame.txt') -ErrorAction SilentlyContinue) +
                   @(Get-Content (Join-Path $script:LogDir 'outgame.err.txt') -ErrorAction SilentlyContinue)
            throw "OutGameServer exited during startup (exit $($process.ExitCode)): $(($log | Select-Object -Last 3) -join ' / ')"
        }
        Start-Sleep -Milliseconds 100
    }
    throw "OutGameServer did not start within $TimeoutSec s"
}

# 아웃게임 서버 API 호출 (POST JSON). 4xx/5xx도 예외 없이 @{ Status; Body } 로 돌려줌
#   주소는 OutGameServer/appsettings.json의 Kestrel 주소
function Invoke-AuthApi
{
    param([string]$Path, [hashtable]$Body)

    # PowerShell 5.1 기본값에는 TLS 1.2가 빠져 있을 수 있음 (Kestrel은 TLS 1.2 이상)
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

    $uri = "https://localhost:7100$Path"
    $json = $Body | ConvertTo-Json -Compress
    try
    {
        $response = Invoke-WebRequest -Uri $uri -Method Post -ContentType 'application/json' -Body $json -UseBasicParsing
        $content = if ($response.Content) { $response.Content | ConvertFrom-Json } else { $null }
        return @{ Status = [int]$response.StatusCode; Body = $content }
    }
    catch [System.Net.WebException]
    {
        $errorResponse = $_.Exception.Response
        if ($null -eq $errorResponse) { throw }   # 연결 자체가 안 됨 (서버 꺼짐, 인증서 문제)

        $reader = New-Object System.IO.StreamReader($errorResponse.GetResponseStream())
        $text = $reader.ReadToEnd()
        $content = if ($text) { $text | ConvertFrom-Json } else { $null }
        return @{ Status = [int]$errorResponse.StatusCode; Body = $content }
    }
}

# 테스트용 Access Token(JWT)을 직접 만듦 (토큰 공격 시나리오용)
#   기본값은 아웃게임 서버가 발급하는 것과 같은 모양: HS256, 게임 서버 config.ini [Auth]의 비밀키, iss/aud, 15분
#   -ExpOffsetSec 음수 = 이미 만료, -Alg none = 서명 없는 토큰, -SecretBase64 = 다른 키로 서명
function New-TestJwt
{
    param(
        [string]$Sub = '1',
        [int]$ExpOffsetSec = 900,
        [ValidateSet('HS256', 'none')]
        [string]$Alg = 'HS256',
        [string]$SecretBase64,
        [string]$Issuer = 'MMO-Auth',
        [string]$Audience = 'MMO-Game'
    )

    function ConvertTo-Base64Url([byte[]]$Bytes)
    {
        return [Convert]::ToBase64String($Bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_')
    }

    if (-not $SecretBase64)
    {
        # 게임 서버가 실제로 읽는 파일 (실행 폴더에 복사된 config.ini)
        $configPath = Join-Path (Get-BinDir) 'config.ini'
        $line = Get-Content $configPath -Encoding UTF8 | Where-Object { $_ -match '^\s*JwtSecret\s*=' } | Select-Object -First 1
        if (-not $line) { throw "JwtSecret not found in $configPath (run OutGameServer/Setup-Secrets.ps1)" }
        $SecretBase64 = ($line -split '=', 2)[1].Trim()
    }

    $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $header = @{ alg = $Alg; typ = 'JWT' } | ConvertTo-Json -Compress
    $payload = [ordered]@{ iss = $Issuer; aud = $Audience; sub = $Sub; iat = $now; nbf = $now; exp = $now + $ExpOffsetSec } | ConvertTo-Json -Compress

    $signingInput = (ConvertTo-Base64Url ([Text.Encoding]::UTF8.GetBytes($header))) + '.' + (ConvertTo-Base64Url ([Text.Encoding]::UTF8.GetBytes($payload)))
    if ($Alg -eq 'none') { return "$signingInput." }

    $hmac = New-Object System.Security.Cryptography.HMACSHA256 (, [Convert]::FromBase64String($SecretBase64))
    $signature = $hmac.ComputeHash([Text.Encoding]::ASCII.GetBytes($signingInput))
    $hmac.Dispose()
    return "$signingInput." + (ConvertTo-Base64Url $signature)
}

# 이름 붙인 DummyClient 그룹 실행 (프로세스 하나에 Count명). 추가 옵션은 -Arguments로 전달
#   계정 이름 = <그룹 이름><시나리오 시각>_<번호> (없으면 DummyClient가 가입부터 함). --token을 넘기면 로그인 없이 그 값으로 입장
function Start-DummyClients
{
    param([string]$Name, [int]$Count = 1, [string]$Arguments = '')

    $null = Start-TestProcess -Name $Name -Exe 'DummyClient.exe' -Arguments "--count $Count --account-prefix $Name$script:RunStamp $Arguments".Trim()
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

# 로그가 아니라 값으로 확인할 때 (API 응답 코드 등)
function Assert-That
{
    param([bool]$Condition, [string]$Message)

    if ($Condition) { Write-Host "  [ok] $Message" -ForegroundColor Green }
    else { Add-TestFailure $Message }
}

function Assert-ProcessAlive
{
    param([string]$Name)

    $process = $script:Processes[$Name]
    if ($process -and -not $process.HasExited) { Write-Host "  [ok] $Name alive" -ForegroundColor Green }
    else { Add-TestFailure "$Name is not running (crashed or exited)" }
}

function Assert-ServerAlive
{
    $process = $script:Processes['server']
    if ($process -and -not $process.HasExited) { Write-Host '  [ok] server alive' -ForegroundColor Green }
    else { Add-TestFailure 'server is not running (crashed or exited)' }
}

Export-ModuleMember -Function *
