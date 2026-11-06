# config.ini [Database]의 Database(mmo_db)를 만든다. 이미 있으면 그대로 둠 -> 여러 번 실행해도 안전
#   처음 한 번, 또는 DB를 지웠을 때:  .\Database\Setup-Database.ps1
#   Test\Run-Test.ps1이 시나리오 전에 자동으로 호출한다
#   테이블은 처음 생기는 단계(스킬 데이터)에서 SQL 파일과 적용 순서를 이 스크립트에 추가
#   스크립트 실행이 막혀 있으면: powershell -ExecutionPolicy Bypass -File .\Database\Setup-Database.ps1
param(
    [string]$ConfigPath = (Join-Path $PSScriptRoot '..\GameServer\config.ini')
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
    $ok = $true
}
catch { Write-Host "[Setup-Database] $($_.Exception.Message)" -ForegroundColor Red }
finally { if ($conn) { $conn.Close() } }

if ($ok) { exit 0 } else { exit 1 }
