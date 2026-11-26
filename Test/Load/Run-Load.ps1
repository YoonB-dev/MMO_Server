# 부하 테스트: 봇 N명 + 몬스터 M마리를 붙여서 틱 처리 시간, 송신량, CPU, 메모리를 잰다 (AOI 적용 전/후 비교용)
#   .\Test\Load\Run-Load.ps1 -Players 100 -Monsters 300 -Label baseline
#   .\Test\Load\Run-Load.ps1 -Players 50 -NoRecord        기록 확인용 (results.csv에 안 남김)
#   .\Test\Load\Run-Load.ps1 -Build ...                   Release 빌드부터 (GameServer, DummyClient, OutGameServer)
#   .\Test\Load\Run-Load.ps1 -Boss -Monsters 0 -Area 15 -Label boss-before
#                                                         보스전: (0, 0)에 TestBoss(HP 5천만) + 봇이 모두 보스에게 붙어서 기본 공격
#   .\Test\Load\Run-Load.ps1 -Monsters 0 -SaveIntervalSec 10 -Wave 400 -Label db-wave
#                                                         DB: 측정 시작 때 두 번째 무리 400명이 한꺼번에 입장 (주기 저장과 겹치는 로그인)
#   .\Test\Load\Run-Load.ps1 -Monsters 0 -Wave 400 -WaveReuse -Label db-reconnect
#                                                         DB: 두 번째 무리가 첫 무리와 같은 계정 = 전원 중복 로그인 (재접속 폭주)
#   스크립트 실행이 막혀 있으면: powershell -ExecutionPolicy Bypass -File .\Test\Load\Run-Load.ps1 ...
#
# 순서: DB 준비 -> OutGameServer + GameServer(--load-monsters) -> 봇 입장 -> 워밍업(맵에 흩어지는 시간) -> 측정 -> 정리
#   봇 계정은 load_1, load_2 ... 를 매번 다시 씀 (없으면 가입부터. 실행마다 계정이 쌓이지 않게)
#
# 남기는 것
#   Test/logs/load/<시각>_<label>_p<N>_m<M>/   원본 (gitignore)
#     env.txt       측정 환경 (CPU, RAM, OS, 빌드 구성, git 커밋, 실행 인자)
#     server.txt    GameServer 로그 전체 ([Room], [DB] 10초 통계 줄)
#     bots.txt      DummyClient 로그
#     bots-wave.txt 두 번째 무리 DummyClient 로그 (-Wave일 때만)
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
    [switch]$NoRecord,
    [switch]$Boss,          # 보스전 (GameServer --load-boss 2, DummyClient --bot-attack). results.csv에는 열이 따로 없어서 -Label로 구분
    [int]$SaveIntervalSec = 0,  # 주기 저장 간격 (GameServer --save-interval-sec). 0이면 서버 기본값(60초) 그대로
    [int]$Wave = 0,             # 측정 시작 때 한꺼번에 들어오는 두 번째 무리 (0이면 없음)
    [switch]$WaveReuse          # 두 번째 무리가 첫 무리와 같은 계정(load_1...) = 전원 중복 로그인 (재접속 폭주)
)

$ErrorActionPreference = 'Stop'
$testDir  = Split-Path $PSScriptRoot -Parent
$repoRoot = Split-Path $testDir -Parent
Import-Module (Join-Path $testDir 'TestLib.psm1') -Force

$runName = '{0}_{1}_p{2}_m{3}' -f (Get-Date -Format 'yyyyMMdd_HHmmss'), $Label, $Players, $Monsters
$runDir  = Join-Path $testDir "logs\load\$runName"
Initialize-TestContext -Configuration $Configuration -LogDir $runDir

$serverArgs = "--load-monsters $Monsters --load-area $Area"
$botCommon  = "--no-ping --bot --area $Area"    # 첫 무리와 두 번째 무리(-Wave)가 같이 쓰는 봇 인자
$bossType   = 2     # Database/Data/monster.sql의 TestBoss
if ($Boss)
{
    $serverArgs += " --load-boss $bossType"
    $botCommon  += ' --bot-attack'
}
$botArgs    = "--count $Players --account-prefix load $botCommon"
$serverDefaultSaveSec = 60  # GameServer.cpp ParseSaveIntervalSec의 기본값 (csv에 실제 간격을 남기려고)
$saveIntervalUsed = if ($SaveIntervalSec -gt 0) { $SaveIntervalSec } else { $serverDefaultSaveSec }
if ($SaveIntervalSec -gt 0) { $serverArgs += " --save-interval-sec $SaveIntervalSec" }

# 두 번째 무리: 첫 무리와 같은 봇. 계정만 다르게(loadw_) 또는 같게(load_, -WaveReuse)
if ($WaveReuse -and $Wave -le 0) { throw '-WaveReuse needs -Wave N' }
if ($WaveReuse -and $Wave -gt $Players) { throw '-WaveReuse: -Wave must be <= -Players (it reuses the first group accounts)' }
$wavePrefix = if ($WaveReuse) { 'load' } else { 'loadw' }
$waveArgs   = "--count $Wave --account-prefix $wavePrefix $botCommon"

# ==================== 측정 환경 (env.txt) ====================

function Get-GitText { param([string[]]$GitArgs) return ((& git -C $repoRoot @GitArgs) -join "`n").Trim() }

$cpuInfo  = Get-CimInstance Win32_Processor | Select-Object -First 1
$osInfo   = Get-CimInstance Win32_OperatingSystem
$ramGb    = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
$commit   = Get-GitText @('rev-parse', '--short', 'HEAD')
$branch   = Get-GitText @('rev-parse', '--abbrev-ref', 'HEAD')
$changed  = @(& git -C $repoRoot status --porcelain --untracked-files=no -- . ':(exclude)Docs/LoadTest/results.csv')   # 커밋 안 된 변경 (새 파일, 이 스크립트가 쓰는 results.csv는 제외)
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
    "wave          $(if ($Wave -gt 0) { "$Wave at measure start$(if ($WaveReuse) { ' (same accounts: duplicate logins)' }), args $waveArgs" } else { 'none' })"
    "save interval $saveIntervalUsed s"
    "players       $Players"
    "monsters      $Monsters (+ data_monster_spawn)$(if ($Boss) { " + boss (type $bossType) at (0, 0)" })"
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

$hitPattern = '\[Room\] skill hits (\d+), skill work ([\d.e+-]+) ms'   # 플레이어 스킬 명중 수와 HandleSkill 처리 시간 (10초 구간, 명중 0이면 안 찍힘)

# 서버 [DB] 10초 통계 한 줄 (DBQueue::LogStats). 일감 종류마다 "건수 (wait avg/max, run avg/max)", save는 저장한 줄 수가 더 있음
$num = '([\d.e+-]+)'
$dbJob = "\(wait avg ${num}ms max ${num}ms, run avg ${num}ms max ${num}ms\)"
$dbPattern = "\[DB\] load (\d+) $dbJob, leave (\d+) $dbJob, save (\d+) \((\d+) rows, wait avg ${num}ms max ${num}ms, run avg ${num}ms max ${num}ms\)"

# 서버 [Room] enter 10초 통계 한 줄 (Room::LogStats, 그 10초에 입장이 있을 때만): 입장 구간 다섯 조각 (ms)
#   db: 받음 -> 불러오기 끝, room wait: Room 줄에서 기다림, run: Room::Enter, send wait: 틱 끝 송신까지, total: 받음 -> 송신
$enterPattern = "\[Room\] enter (\d+) \(db avg ${num} max ${num}, room wait avg ${num} max ${num}, run avg ${num} max ${num}" +
                ", send wait avg ${num} max ${num}, total avg ${num} max ${num}\) ms"

$samples   = New-Object System.Collections.Generic.List[object]
$roomRows  = @()
$hitRows   = @()
$enterMs   = @()    # 성공한 입장마다 봇이 잰 입장 지연 (ms, 작은 순으로 정렬)
$waveMs    = @()    # 두 번째 무리의 입장 지연 (같은 방식)
$entered   = 0
$waveEntered = 0
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

    # 입장 지연: DummyClient가 성공한 S_ENTER_GAME마다 찍은 enter_ms (C_ENTER_GAME을 보낸 뒤 응답까지)
    $enterMs = @(Select-String -Path (Join-Path $runDir 'bots.txt') -Pattern 'S_ENTER_GAME success=1 enter_ms=([\d.]+)' |
        ForEach-Object { [double]$_.Matches[0].Groups[1].Value } | Sort-Object)

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

    # 두 번째 무리: 측정 시작과 동시에 한꺼번에 (첫 무리가 이미 방에 있고, 주기 저장도 도는 중)
    if ($Wave -gt 0)
    {
        $null = Start-TestProcess -Name 'bots-wave' -Exe 'DummyClient.exe' -Arguments $waveArgs
        Write-Host "  wave: $Wave bots started$(if ($WaveReuse) { ' (same accounts as the first group)' })"
    }

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
            $row = [pscustomobject]@{
                IntervalMs = [double]$Matches[2]
                WorkAvgMs  = [double]$Matches[3]
                WorkMaxMs  = [double]$Matches[4]
                Players    = [int]$Matches[6]
                Monsters   = [int]$Matches[7]
                Packets    = [double]$Matches[8]
                KB         = [double]$Matches[9]
                IntervalPeakMs = 0
            }
            # 틱 하나의 가장 긴 간격 (줄 끝, 예전 로그에는 없음 -> 0)
            #   두 번째 -match가 $Matches를 덮어쓰므로 위에서 값을 다 꺼낸 다음에
            if ($_ -match 'interval max ([\d.e+-]+)ms') { $row.IntervalPeakMs = [double]$Matches[1] }
            $row
        }
    })
    $hitRows = @(Get-Content $serverLog | Select-Object -Skip $skipLines | ForEach-Object {
        if ($_ -match $hitPattern) { [pscustomobject]@{ Hits = [double]$Matches[1]; WorkMs = [double]$Matches[2] } }
    })

    # 두 번째 무리가 측정 안에 다 못 들어왔으면 조금 더 기다림 (가입/로그인이 인원에 비례). 그래도 모자라면 아래 problems에 남음
    #   [Room] 줄은 위에서 이미 읽어서, 이 기다림은 측정 값에 안 섞임 ([DB] 줄은 실행 전체를 보므로 들어감)
    if ($Wave -gt 0)
    {
        $late = (Get-LogMatchCount -Name 'bots-wave' -Pattern 'S_ENTER_GAME success=1') -lt $Wave
        if ($late -and -not $server.HasExited)
        {
            Wait-LogCount -Name 'bots-wave' -Pattern 'S_ENTER_GAME success=1' -Expected $Wave -TimeoutSec ([math]::Max(60, $Wave))
            Wait-Seconds 11     # 마지막 입장들이 들어간 [DB] 10초 구간이 찍힐 때까지 (서버를 끄면 그 구간은 안 찍힘)
        }
        $waveEntered = Get-LogMatchCount -Name 'bots-wave' -Pattern 'S_ENTER_GAME success=1'
        Write-Host "  wave entered: $waveEntered / $Wave"
        $waveMs = @(Select-String -Path (Join-Path $runDir 'bots-wave.txt') -Pattern 'S_ENTER_GAME success=1 enter_ms=([\d.]+)' |
            ForEach-Object { [double]$_.Matches[0].Groups[1].Value } | Sort-Object)
    }
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

# 백분위 (nearest-rank): 정렬된 값에서 p% 위치의 값. p99 = 100명을 빠른 순으로 세웠을 때 99번째 사람의 시간
#   실제로 잰 값 중 하나를 고름 (두 값 사이를 보간하지 않음). 400명이면 p99 = 396번째, p100 = 가장 느린 사람
function Get-Percentile
{
    param([double[]]$Sorted, [double]$P)
    if ($Sorted.Count -eq 0) { return 0 }
    $index = [math]::Ceiling($P / 100 * $Sorted.Count) - 1
    return $Sorted[[math]::Max(0, $index)]
}

# [DB] 줄 전부 (실행 전체): 첫 무리의 로그인은 워밍업 전이고, [DB] 10초 구간은 측정 구간과 시작 시각이 달라서 자르지 않음
#   서버를 강제로 끄므로 마지막 구간(정리할 때의 나감 저장)은 안 찍힘
$serverLogPath = Join-Path $runDir 'server.txt'
$dbRows = @()
if (Test-Path $serverLogPath)
{
    $dbRows = @(Get-Content $serverLogPath | ForEach-Object {
        if ($_ -match $dbPattern)
        {
            [pscustomobject]@{
                LoadCount  = [double]$Matches[1];  LoadWaitAvg  = [double]$Matches[2];  LoadWaitMax  = [double]$Matches[3]
                LoadRunAvg = [double]$Matches[4];  LoadRunMax   = [double]$Matches[5]
                LeaveCount = [double]$Matches[6];  LeaveWaitAvg = [double]$Matches[7];  LeaveWaitMax = [double]$Matches[8]
                LeaveRunAvg = [double]$Matches[9]; LeaveRunMax  = [double]$Matches[10]
                SaveCount  = [double]$Matches[11]; SaveRows     = [double]$Matches[12]
                SaveWaitAvg = [double]$Matches[13]; SaveWaitMax = [double]$Matches[14]
                SaveRunAvg = [double]$Matches[15]; SaveRunMax   = [double]$Matches[16]
            }
        }
    })
}

# [Room] enter 줄: 측정 시작 때의 로그 줄 수($skipLines) 앞 = 첫 무리, 뒤 = 두 번째 무리 (측정 중에는 다른 입장이 없음)
#   두 번째 무리는 위에서 더 기다린 시간까지 들어감 ([DB] 줄과 같음)
$enterFirst = @()
$enterWave = @()
if (Test-Path $serverLogPath)
{
    $lineNo = 0
    foreach ($line in Get-Content $serverLogPath)
    {
        $lineNo++
        if ($line -notmatch $enterPattern) { continue }
        $row = [pscustomobject]@{
            Count       = [double]$Matches[1]
            DbAvg       = [double]$Matches[2];  DbMax       = [double]$Matches[3]
            RoomWaitAvg = [double]$Matches[4];  RoomWaitMax = [double]$Matches[5]
            RunAvg      = [double]$Matches[6];  RunMax      = [double]$Matches[7]
            SendWaitAvg = [double]$Matches[8];  SendWaitMax = [double]$Matches[9]
            TotalAvg    = [double]$Matches[10]; TotalMax    = [double]$Matches[11]
        }
        if ($lineNo -le $skipLines) { $enterFirst += $row } else { $enterWave += $row }
    }
}

# 여러 10초 줄의 평균을 합침: 줄마다 "평균 x 건수"를 더해서 전체 건수로 나눔 (가중 평균. 건수가 많은 구간이 더 무겁게)
function Get-WeightedAvg
{
    param($Rows, [string]$CountProp, [string]$AvgProp)
    $total = ($Rows | ForEach-Object { $_.$CountProp } | Measure-Object -Sum).Sum
    if (-not $total) { return 0 }
    $weighted = ($Rows | ForEach-Object { $_.$CountProp * $_.$AvgProp } | Measure-Object -Sum).Sum
    return $weighted / $total
}
function Get-Sum { param($Values) if ($null -eq $Values -or @($Values).Count -eq 0) { return 0 } return ($Values | Measure-Object -Sum).Sum }

# [Room] 한 줄 = 200틱 구간. 틱이 밀리면 10초보다 길어지므로 실제 길이 = 200틱 x 그 줄의 평균 틱 간격
#   (10초로 치면 틱이 321ms로 밀린 보스전 400명에서 초당 값이 6.4배 부풀려짐)
$roomSec = ($roomRows | ForEach-Object { 200 * $_.IntervalMs / 1000 } | Measure-Object -Sum).Sum
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
    # 새 열은 항상 여기(맨 끝)에만 추가 -> 아래 results.csv 머리줄 갱신이 옛 줄을 빈 칸으로 채움
    enter_p50_ms         = Round2 (Get-Percentile $enterMs 50)
    enter_p95_ms         = Round2 (Get-Percentile $enterMs 95)
    enter_p99_ms         = Round2 (Get-Percentile $enterMs 99)
    enter_max_ms         = Round2 (Get-Percentile $enterMs 100)
    # DB 줄 (4단계): 상황 -> 두 번째 무리의 입장 지연 -> [DB] 통계 (실행 전체)
    save_interval_s      = $saveIntervalUsed
    wave_players         = $Wave
    wave_reuse           = [bool]$WaveReuse
    wave_entered         = $waveEntered
    wave_enter_p50_ms    = Round2 (Get-Percentile $waveMs 50)
    wave_enter_p95_ms    = Round2 (Get-Percentile $waveMs 95)
    wave_enter_p99_ms    = Round2 (Get-Percentile $waveMs 99)
    wave_enter_max_ms    = Round2 (Get-Percentile $waveMs 100)
    db_load_count        = Get-Sum $dbRows.LoadCount
    db_load_wait_avg_ms  = Round2 (Get-WeightedAvg $dbRows 'LoadCount' 'LoadWaitAvg')
    db_load_wait_max_ms  = Round2 (Get-Max $dbRows.LoadWaitMax)
    db_load_run_avg_ms   = Round2 (Get-WeightedAvg $dbRows 'LoadCount' 'LoadRunAvg')
    db_leave_count       = Get-Sum $dbRows.LeaveCount
    db_leave_run_avg_ms  = Round2 (Get-WeightedAvg $dbRows 'LeaveCount' 'LeaveRunAvg')
    db_save_count        = Get-Sum $dbRows.SaveCount
    db_save_rows         = Get-Sum $dbRows.SaveRows
    db_save_run_max_ms   = Round2 (Get-Max $dbRows.SaveRunMax)
    # 서버 안 입장 구간 ([Room] enter 줄): 틱 하나의 최대 간격 -> 첫 무리 8개 -> 두 번째 무리 8개
    tick_interval_peak_ms  = Round2 (Get-Max $roomRows.IntervalPeakMs)
    enter_server_avg_ms    = Round2 (Get-WeightedAvg $enterFirst 'Count' 'TotalAvg')
    enter_server_max_ms    = Round2 (Get-Max $enterFirst.TotalMax)
    enter_db_avg_ms        = Round2 (Get-WeightedAvg $enterFirst 'Count' 'DbAvg')
    enter_room_wait_avg_ms = Round2 (Get-WeightedAvg $enterFirst 'Count' 'RoomWaitAvg')
    enter_room_wait_max_ms = Round2 (Get-Max $enterFirst.RoomWaitMax)
    enter_run_avg_ms       = Round2 (Get-WeightedAvg $enterFirst 'Count' 'RunAvg')
    enter_send_wait_avg_ms = Round2 (Get-WeightedAvg $enterFirst 'Count' 'SendWaitAvg')
    enter_send_wait_max_ms = Round2 (Get-Max $enterFirst.SendWaitMax)
    wave_server_avg_ms     = Round2 (Get-WeightedAvg $enterWave 'Count' 'TotalAvg')
    wave_server_max_ms     = Round2 (Get-Max $enterWave.TotalMax)
    wave_db_avg_ms         = Round2 (Get-WeightedAvg $enterWave 'Count' 'DbAvg')
    wave_room_wait_avg_ms  = Round2 (Get-WeightedAvg $enterWave 'Count' 'RoomWaitAvg')
    wave_room_wait_max_ms  = Round2 (Get-Max $enterWave.RoomWaitMax)
    wave_run_avg_ms        = Round2 (Get-WeightedAvg $enterWave 'Count' 'RunAvg')
    wave_send_wait_avg_ms  = Round2 (Get-WeightedAvg $enterWave 'Count' 'SendWaitAvg')
    wave_send_wait_max_ms  = Round2 (Get-Max $enterWave.SendWaitMax)
}

$summary = @("run $runName", '') + ($result.GetEnumerator() | ForEach-Object { '{0,-22} {1}' -f $_.Key, $_.Value })
$summary += ''
if ($Boss)
{
    # [Room] 줄과 같은 10초 구간 (results.csv에는 열이 없어서 summary에만)
    $hitsPerSec   = if ($roomSec) { [math]::Round(($hitRows.Hits | Measure-Object -Sum).Sum / $roomSec) } else { 0 }
    $skillMsPerSec = if ($roomSec) { Round2 (($hitRows.WorkMs | Measure-Object -Sum).Sum / $roomSec) } else { 0 }
    $summary += "skill hits/s           $hitsPerSec (boss fight: player basic attacks that hit)"
    $summary += "skill work ms/s        $skillMsPerSec (time in HandleSkill per second, outside the tick work; 1000 = Room thread busy all the time)"
    $summary += ''
}
$summary += "[Room] lines used ($($roomRows.Count), each = 200 ticks = 10 s at 50 ms, $([math]::Round($roomSec)) s in total; the first one may start before the measure window):"
$summary += $roomRows | ForEach-Object {
    "  interval $($_.IntervalMs)ms, work avg $($_.WorkAvgMs)ms max $($_.WorkMaxMs)ms, players $($_.Players), monsters $($_.Monsters), sent $($_.Packets) packets $($_.KB) KB"
}
$summary += ''
$summary += "[DB] lines used ($($dbRows.Count), whole run, 10 s each, only intervals with DB jobs):"
$summary += $dbRows | ForEach-Object {
    "  load $($_.LoadCount) (wait avg $($_.LoadWaitAvg) max $($_.LoadWaitMax), run avg $($_.LoadRunAvg) max $($_.LoadRunMax))" +
    ", leave $($_.LeaveCount) (wait max $($_.LeaveWaitMax), run avg $($_.LeaveRunAvg))" +
    ", save $($_.SaveCount) ($($_.SaveRows) rows, wait max $($_.SaveWaitMax), run max $($_.SaveRunMax)) ms"
}
$summary += ''
$summary += "[Room] enter lines used (10 s each, only intervals with entries; first group = before the measure window, wave = after):"
$enterLine = {
    param($Group, $Row)
    "  $Group $($Row.Count) (db avg $($Row.DbAvg) max $($Row.DbMax), room wait avg $($Row.RoomWaitAvg) max $($Row.RoomWaitMax)" +
    ", run avg $($Row.RunAvg) max $($Row.RunMax), send wait avg $($Row.SendWaitAvg) max $($Row.SendWaitMax)" +
    ", total avg $($Row.TotalAvg) max $($Row.TotalMax)) ms"
}
$summary += $enterFirst | ForEach-Object { & $enterLine 'first' $_ }
$summary += $enterWave | ForEach-Object { & $enterLine 'wave ' $_ }
$summary | Set-Content -Path (Join-Path $runDir 'summary.txt') -Encoding UTF8

Write-Host ''
$summary | ForEach-Object { Write-Host $_ }

$problems = @()
if ($entered -lt $Players) { $problems += "only $entered / $Players bots entered" }
if (-not $alive)           { $problems += 'server exited' }
if ($roomRows.Count -eq 0) { $problems += 'no [Room] stats line in the measure window' }
if ($Wave -gt 0 -and $waveEntered -lt $Wave) { $problems += "only $waveEntered / $Wave wave bots entered" }
if ($dbRows.Count -eq 0)   { $problems += 'no [DB] stats line in the server log' }
foreach ($problem in $problems) { Write-Host "WARNING: $problem" -ForegroundColor Yellow }

if ($NoRecord)
{
    Write-Host "Not recorded (-NoRecord). Logs: $runDir"
    exit 0
}

$csvDir  = Join-Path $repoRoot 'Docs\LoadTest'
$csvPath = Join-Path $csvDir 'results.csv'
New-Item -ItemType Directory -Force $csvDir | Out-Null
$header  = $result.Keys -join ','
if (-not (Test-Path $csvPath)) { $header | Set-Content -Path $csvPath -Encoding ASCII }

# 열이 늘었으면(새 열은 항상 맨 끝에만 추가) 머리줄을 바꾸고 옛 줄은 빈 칸으로 채움 -> 모든 줄의 열 수가 같게
#   옛 줄은 그 값을 재지 않았으므로 비워 둠 (숫자를 만들어 넣지 않음)
#   중간에 열을 끼워 넣으면 옛 숫자가 다른 열로 밀리므로 그때는 멈춤
$lines = @(Get-Content $csvPath)
if ($lines[0] -ne $header)
{
    if ($header.StartsWith($lines[0] + ',') -eq $false) { throw 'results.csv header does not match (new columns must be added at the end)' }
    $pad   = ',' * ($result.Count - $lines[0].Split(',').Count)
    $lines = @($header) + @($lines | Select-Object -Skip 1 | ForEach-Object { $_ + $pad })
    $lines | Set-Content -Path $csvPath -Encoding ASCII
    Write-Host "results.csv: added $($pad.Length) column(s) to the header, old rows left empty"
}

(($result.Values | ForEach-Object { "$_" }) -join ',') | Add-Content -Path $csvPath -Encoding ASCII

Write-Host "Recorded: Docs\LoadTest\results.csv"
Write-Host "Logs: $runDir"
