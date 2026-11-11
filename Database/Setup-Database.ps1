# config.ini [Database]의 Database(mmo_db)를 준비한다. 여러 번 실행해도 안전
#   1. DB가 없으면 만든다 (이미 있으면 그대로 둠)
#   2. 기획 데이터 Data/*.sql을 이름 순서대로 다시 적용한다 (파일마다 테이블을 지우고 다시 만들고 값 넣기)
#      -> 기획 데이터의 정답은 이 파일들. 값을 바꾸려면 DB가 아니라 파일을 고치고 이 스크립트를 다시 실행
#   처음 한 번, 또는 Data/*.sql을 고쳤을 때:  .\Database\Setup-Database.ps1
#   Test\Run-Test.ps1이 시나리오 전에 자동으로 호출한다
#   -ExtraSql: Data/*.sql 적용 뒤에 실행할 문장들 (테스트 시나리오가 수치를 잠깐 덮어쓸 때, TestLib의 Set-GameData)
#   스크립트 실행이 막혀 있으면: powershell -ExecutionPolicy Bypass -File .\Database\Setup-Database.ps1
param(
    [string]$ConfigPath = (Join-Path $PSScriptRoot '..\GameServer\config.ini'),
    [string[]]$ExtraSql = @()
)

$ErrorActionPreference = 'Stop'

# 서버의 ConfigManager와 같은 키를 읽음
function Read-DbConfig
{
    param([string]$Path)
    if (-not (Test-Path $Path)) { throw "config.ini not found: $Path (copy GameServer/config.example.ini to GameServer/config.ini)" }

    $db = @{}
    $section = ''
    foreach ($raw in Get-Content $Path -Encoding UTF8)
    {
        $line = $raw.Trim()
        if ($line -eq '' -or $line.StartsWith('#') -or $line.StartsWith(';')) { continue }
        if ($line.StartsWith('[')) { $section = $line; continue }
        if ($section -eq '[Database]' -and $line -match '^([^=]+)=(.*)$') { $db[$matches[1].Trim()] = $matches[2].Trim() }
    }
    foreach ($key in 'Driver', 'Server', 'Port', 'Database', 'UID', 'PWD')
    {
        if (-not $db.ContainsKey($key)) { throw "config.ini [Database] is missing '$key'" }
    }
    return $db
}

# SQL 파일을 문장 단위로 나눠서 하나씩 실행 -> 틀린 문장이 몇 번째인지 알 수 있음
#   규칙: 문장은 줄 끝의 ;로 끝남 (; 뒤에 주석 금지), 줄 전체 주석(--)은 건너뜀
function Invoke-SqlFile
{
    param([System.Data.Odbc.OdbcConnection]$Connection, [string]$Path)

    $lines = Get-Content $Path -Encoding UTF8 | Where-Object { -not $_.Trim().StartsWith('--') }
    $statements = @(($lines -join "`n") -split ';[ \t]*(?:\n|$)' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })

    $cmd = $Connection.CreateCommand()
    for ($i = 0; $i -lt $statements.Count; $i++)
    {
        $cmd.CommandText = $statements[$i]
        try { [void]$cmd.ExecuteNonQuery() }
        catch
        {
            $reason = if ($_.Exception.InnerException) { $_.Exception.InnerException.Message } else { $_.Exception.Message }
            $firstLine = ($statements[$i] -split "`n")[0]
            throw "$(Split-Path $Path -Leaf) statement $($i + 1) '$firstLine': $reason"
        }
    }
    return $statements.Count
}

$conn = $null
$ok = $false
try
{
    $db = Read-DbConfig $ConfigPath
    $name = $db['Database']

    # DB 이름은 SQL 문장에 그대로 들어가므로 영문/숫자/_만 허용
    if ($name -notmatch '^[A-Za-z0-9_]+$') { throw "Invalid database name '$name' (letters, digits, _ only)" }

    # DATABASE 없이 MySQL에만 접속 (아직 DB가 없을 수 있으므로). 비밀번호가 들어 있어서 연결 문자열은 출력하지 않음
    $connStr = "DRIVER={$($db['Driver'])};SERVER=$($db['Server']);PORT=$($db['Port']);UID=$($db['UID']);PWD=$($db['PWD']);"
    $conn = New-Object System.Data.Odbc.OdbcConnection($connStr)
    try { $conn.Open() }
    catch
    {
        $reason = if ($_.Exception.InnerException) { $_.Exception.InnerException.Message } else { $_.Exception.Message }
        throw "Cannot connect to MySQL at $($db['Server']):$($db['Port']) - $reason (is the MySQL service running?)"
    }

    $cmd = $conn.CreateCommand()
    $cmd.CommandText = "SELECT COUNT(*) FROM information_schema.SCHEMATA WHERE SCHEMA_NAME = '$name'"
    if ([int]$cmd.ExecuteScalar() -gt 0)
    {
        Write-Host "Database '$name' already exists"
    }
    else
    {
        # utf8mb4 = 진짜 UTF-8 (MySQL의 utf8은 3바이트까지라 이모지를 못 넣음). MySQL 8 기본값이지만 명시
        $cmd.CommandText = "CREATE DATABASE $name CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci"
        [void]$cmd.ExecuteNonQuery()
        Write-Host "Database '$name' created (utf8mb4)"
    }

    # 기획 데이터: Data/*.sql을 이름 순서대로 적용 (파일마다 지우고 다시 만들고 값 넣기)
    $cmd.CommandText = "USE $name"
    [void]$cmd.ExecuteNonQuery()
    foreach ($file in Get-ChildItem (Join-Path $PSScriptRoot 'Data') -Filter '*.sql' | Sort-Object Name)
    {
        $count = Invoke-SqlFile -Connection $conn -Path $file.FullName
        Write-Host "Applied Data/$($file.Name) ($count statements)"
    }

    # 테스트용 덮어쓰기 (Data/*.sql을 다시 적용하면 원래 값으로 돌아감)
    foreach ($sql in $ExtraSql)
    {
        $cmd.CommandText = $sql
        try { [void]$cmd.ExecuteNonQuery() }
        catch
        {
            $reason = if ($_.Exception.InnerException) { $_.Exception.InnerException.Message } else { $_.Exception.Message }
            throw "extra SQL '$sql': $reason"
        }
        Write-Host "Applied extra SQL: $sql"
    }
    $ok = $true
}
catch { Write-Host "[Setup-Database] $($_.Exception.Message)" -ForegroundColor Red }
finally { if ($conn) { $conn.Close() } }

if ($ok) { exit 0 } else { exit 1 }
