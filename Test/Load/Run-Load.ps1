# 부하 테스트: 봇 N명 + 몬스터 M마리를 붙여서 틱 처리 시간, 송신량, CPU, 메모리를 잰다 (AOI 적용 전/후 비교용)
#   .\Test\Load\Run-Load.ps1 -Players 100 -Monsters 300 -Label baseline
#   .\Test\Load\Run-Load.ps1 -Players 50 -NoRecord        기록 확인용 (results.csv에 안 남김)
#   .\Test\Load\Run-Load.ps1 -Build ...                   Release 빌드부터 (GameServer, DummyClient, OutGameServer)
#   스크립트 실행이 막혀 있으면: powershell -ExecutionPolicy Bypass -File .\Test\Load\Run-Load.ps1 ...
#
# 순서: DB 준비 -> OutGameServer + GameServer(--load-monsters) -> 봇 입장 -> 워밍업(맵에 흩어지는 시간) -> 측정 -> 정리
#   봇 계정은 load_1, load_2 ... 를 매번 다시 씀 (없으면 가입부터. 실행마다 계정이 쌓이지 않게)
#
# 남기는 것
#   Test/logs/load/<시각>_<label>_p<N>_m<M>/   원본 (gitignore)
#     env.txt       측정 환경 (CPU, RAM, OS, 빌드 구성, git 커밋, 실행 인자)
#     server.txt    GameServer 로그 전체 ([Room] 10초 통계 줄)
#     bots.txt      DummyClient 로그
#     outgame.txt   OutGameServer 로그
#     process.csv   1초마다 서버 프로세스 CPU / 메모리 / 스레드 수
#     summary.txt   측정 구간만 계산한 결과
#   Docs/LoadTest/results.csv                  실행마다 한 줄 (커밋 대상. -NoRecord면 안 남김)
param(
    [int]$Players = 100,
    [int]$Monsters = 300,
    [double]$Area = 100,
    [int]$WarmupSec = 60,
    [int]$MeasureSec = 60,
    [string]$Label = 'baseline',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Build,
    [switch]$NoRecord
)

$ErrorActionPreference = 'Stop'
$testDir  = Split-Path $PSScriptRoot -Parent
$repoRoot = Split-Path $testDir -Parent
Import-Module (Join-Path $testDir 'TestLib.psm1') -Force

$runName = '{0}_{1}_p{2}_m{3}' -f (Get-Date -Format 'yyyyMMdd_HHmmss'), $Label, $Players, $Monsters
$runDir  = Join-Path $testDir "logs\load\$runName"
Initialize-TestContext -Configuration $Configuration -LogDir $runDir

$serverArgs = "--load-monsters $Monsters --load-area $Area"
$botArgs    = "--count $Players --account-prefix load --no-ping --bot --area $Area"

# ==================== 측정 환경 (env.txt) ====================

function Get-GitText { param([string[]]$GitArgs) return ((& git -C $repoRoot @GitArgs) -join "`n").Trim() }

$cpuInfo  = Get-CimInstance Win32_Processor | Select-Object -First 1
$osInfo   = Get-CimInstance Win32_OperatingSystem
$ramGb    = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
$commit   = Get-GitText @('rev-parse', '--short', 'HEAD')
$branch   = Get-GitText @('rev-parse', '--abbrev-ref', 'HEAD')
$changed  = @(& git -C $repoRoot status --porcelain --untracked-files=no)   # 커밋 안 된 변경 (새 파일은 제외)
$dirty    = $changed.Count -gt 0
$started  = Get-Date

$envLines = @(
    "run           $runName"
    "started       $($started.ToString('yyyy-MM-dd HH:mm:ss')) (local), $($started.ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ss')) UTC"
    "cpu           $($cpuInfo.Name.Trim()) ($($cpuInfo.NumberOfCores) cores, $($cpuInfo.NumberOfLogicalProcessors) threads)"
    "ram           $ramGb GB"
    "os            $($osInfo.Caption) (build $($osInfo.BuildNumber))"
    "build         $Configuration|x64"
    "git           $branch @ $commit$(if ($dirty) { ' (uncommitted changes)' })"
    "server args   $serverArgs"
    "bot args      $botArgs"
    "players       $Players"
    "monsters      $Monsters (+ data_monster_spawn)"
    "area          +-$Area m"
    "warmup        $WarmupSec s"
    "measure       $MeasureSec s"
    "note          bots run on the same PC as the server"
)
if ($dirty) { $envLines += 'changed files:'; $envLines += $changed | ForEach-Object { "  $_" } }
$envLines | Set-Content -Path (Join-Path $runDir 'env.txt') -Encoding UTF8
$envLines | ForEach-Object { Write-Host $_ }
if ($dirty) { Write-Host 'WARNING: uncommitted changes - this result cannot be tied to a commit' -ForegroundColor Yellow }

# ==================== 실행 ====================

# 서버 [Room] 10초 통계 한 줄 (Room::LogStats)
$roomPattern = '\[Room\] tick (\d+), avg interval ([\d.e+-]+)ms, work avg ([\d.e+-]+)ms max ([\d.e+-]+)ms, ' +
               'monster moves (\d+) \([^)]*\), players (\d+), monsters (\d+), sent (\d+) packets (\d+) KB'

$samples   = New-Object System.Collections.Generic.List[object]
$roomRows  = @()
$entered   = 0
$alive     = $false

try
{
    if ($Build) { Invoke-SolutionBuild -Configuration $Configuration }

    & (Join-Path $repoRoot 'Database\Setup-Database.ps1') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Database setup failed (is MySQL80 running?)' }

    Start-GameServer -Arguments $serverArgs -TimeoutSec 30
    $serverExe = Join-Path (Get-BinDir) 'GameServer.exe'
    $server = Get-Process -Name GameServer | Where-Object { $_.Path -eq $serverExe } | Sort-Object StartTime -Descending | Select-Object -First 1

    $null = Start-TestProcess -Name 'bots' -Exe 'DummyClient.exe' -Arguments $botArgs
    $enterTimeout = [math]::Max(60, $Players)   # 가입/로그인(PBKDF2)이 한 명당 수십 ms라 인원에 비례
    Write-Host "  waiting for $Players bots to enter (up to $enterTimeout s) ..."
    Wait-LogCount -Name 'bots' -Pattern 'S_ENTER_GAME success=1' -Expected $Players -TimeoutSec $enterTimeout
    $entered = Get-LogMatchCount -Name 'bots' -Pattern 'S_ENTER_GAME success=1'
    Write-Host "  entered: $entered / $Players"

    Write-Host "  warmup $WarmupSec s (bots spread over the map) ..."
    Wait-Seconds $WarmupSec
    if ($server.HasExited) { throw 'GameServer exited during warmup' }

    # 측정: 지금부터 서버 로그에 새로 찍히는 [Room] 줄 + 1초마다 프로세스 샘플
    $serverLog  = Join-Path $runDir 'server.txt'
    $skipLines  = @(Get-Content $serverLog).Count
    $cores      = [Environment]::ProcessorCount
    $server.Refresh()
    $prevCpu    = $server.TotalProcessorTime
    $prevTime   = Get-Date

    Write-Host "  measuring $MeasureSec s ..."
    for ($sec = 1; $sec -le $MeasureSec; $sec++)
    {
        Start-Sleep -Seconds 1
        if ($server.HasExited) { break }

        $server.Refresh()
        $now     = Get-Date
        $cpuTime = $server.TotalProcessorTime
        $used    = ($cpuTime - $prevCpu).TotalSeconds / ($now - $prevTime).TotalSeconds   # 코어 몇 개분을 썼나
        $samples.Add([pscustomobject]@{
            sec            = $sec
            cpu_cores      = [math]::Round($used, 3)
            cpu_pct        = [math]::Round($used / $cores * 100, 2)
            working_set_mb = [math]::Round($server.WorkingSet64 / 1MB, 1)
            private_mb     = [math]::Round($server.PrivateMemorySize64 / 1MB, 1)
            threads        = $server.Threads.Count
        })
        $prevCpu  = $cpuTime
        $prevTime = $now
    }

    $alive = -not $server.HasExited
    $roomRows = @(Get-Content $serverLog | Select-Object -Skip $skipLines | ForEach-Object {
        if ($_ -match $roomPattern)
        {
            [pscustomobject]@{
                IntervalMs = [double]$Matches[2]
                WorkAvgMs  = [double]$Matches[3]
                WorkMaxMs  = [double]$Matches[4]
                Players    = [int]$Matches[6]
                Monsters   = [int]$Matches[7]
                Packets    = [double]$Matches[8]
                KB         = [double]$Matches[9]
            }
        }
    })
}
finally
{
    Stop-AllTestProcesses
}

# ==================== 결과 (summary.txt, results.csv) ====================

$samples | Export-Csv -Path (Join-Path $runDir 'process.csv') -NoTypeInformation -Encoding ASCII

function Get-Avg { param($Values) if ($null -eq $Values -or @($Values).Count -eq 0) { return 0 } return ($Values | Measure-Object -Average).Average }
function Get-Max { param($Values) if ($null -eq $Values -or @($Values).Count -eq 0) { return 0 } return ($Values | Measure-Object -Maximum).Maximum }
function Round2  { param([double]$Value) return [math]::Round($Value, 2) }

$roomSec = $roomRows.Count * 10   # [Room] 한 줄 = 10초(200틱) 구간
$result = [ordered]@{
    date                 = $started.ToString('yyyy-MM-dd HH:mm')
    label                = $Label
    commit               = $commit
    dirty                = $dirty
    config               = $Configuration
    players              = $Players
    players_entered      = $entered
    players_in_room      = if ($roomRows.Count) { $roomRows[-1].Players } else { 0 }
    monsters_in_room     = if ($roomRows.Count) { $roomRows[-1].Monsters } else { 0 }
    area_m               = $Area
    warmup_s             = $WarmupSec
    measure_s            = $MeasureSec
    room_samples         = $roomRows.Count
    tick_interval_avg_ms = Round2 (Get-Avg $roomRows.IntervalMs)
    tick_interval_max_ms = Round2 (Get-Max $roomRows.IntervalMs)
    work_avg_ms          = Round2 (Get-Avg $roomRows.WorkAvgMs)
    work_max_ms          = Round2 (Get-Max $roomRows.WorkMaxMs)
    packets_per_s        = if ($roomSec) { [math]::Round(($roomRows.Packets | Measure-Object -Sum).Sum / $roomSec) } else { 0 }
    kb_per_s             = if ($roomSec) { [math]::Round(($roomRows.KB | Measure-Object -Sum).Sum / $roomSec) } else { 0 }
    cpu_cores_avg        = Round2 (Get-Avg $samples.cpu_cores)
    cpu_cores_max        = Round2 (Get-Max $samples.cpu_cores)
    cpu_pct_avg          = Round2 (Get-Avg $samples.cpu_pct)
    mem_mb_avg           = Round2 (Get-Avg $samples.working_set_mb)
    mem_mb_max           = Round2 (Get-Max $samples.working_set_mb)
    server_alive         = $alive
    run_dir              = $runName
}

$summary = @("run $runName", '') + ($result.GetEnumerator() | ForEach-Object { '{0,-22} {1}' -f $_.Key, $_.Value })
$summary += ''
$summary += "[Room] lines used ($($roomRows.Count), each = 10 s; the first one may start up to 10 s before the measure window):"
$summary += $roomRows | ForEach-Object {
    "  interval $($_.IntervalMs)ms, work avg $($_.WorkAvgMs)ms max $($_.WorkMaxMs)ms, players $($_.Players), monsters $($_.Monsters), sent $($_.Packets) packets $($_.KB) KB"
}
$summary | Set-Content -Path (Join-Path $runDir 'summary.txt') -Encoding UTF8

Write-Host ''
$summary | ForEach-Object { Write-Host $_ }

$problems = @()
if ($entered -lt $Players) { $problems += "only $entered / $Players bots entered" }
if (-not $alive)           { $problems += 'server exited' }
if ($roomRows.Count -eq 0) { $problems += 'no [Room] stats line in the measure window' }
foreach ($problem in $problems) { Write-Host "WARNING: $problem" -ForegroundColor Yellow }

if ($NoRecord)
{
    Write-Host "Not recorded (-NoRecord). Logs: $runDir"
    exit 0
}

$csvDir  = Join-Path $repoRoot 'Docs\LoadTest'
$csvPath = Join-Path $csvDir 'results.csv'
New-Item -ItemType Directory -Force $csvDir | Out-Null
if (-not (Test-Path $csvPath)) { ($result.Keys -join ',') | Set-Content -Path $csvPath -Encoding ASCII }
(($result.Values | ForEach-Object { "$_" }) -join ',') | Add-Content -Path $csvPath -Encoding ASCII

Write-Host "Recorded: Docs\LoadTest\results.csv"
Write-Host "Logs: $runDir"
