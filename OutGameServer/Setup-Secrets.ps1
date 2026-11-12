# OutGameServer의 비밀값을 dotnet user-secrets에 저장한다 (저장소 밖: %APPDATA%\Microsoft\UserSecrets\<UserSecretsId>\secrets.json)
#   ConnectionStrings:Mmo : GameServer/config.ini [Database]로 만듦 (DB 접속 정보는 config.ini 한 곳에서만 관리)
#   Auth:JwtSecret        : 없으면 랜덤 32바이트로 새로 만들고, 있으면 그대로 둠 (바꾸면 이미 발급한 토큰이 전부 무효)
#   값은 화면에 출력하지 않음. 처음 한 번, 또는 config.ini의 DB 정보를 바꿨을 때 실행
#   스크립트 실행이 막혀 있으면: powershell -ExecutionPolicy Bypass -File .\OutGameServer\Setup-Secrets.ps1
param(
    [string]$ConfigPath = (Join-Path $PSScriptRoot '..\GameServer\config.ini')
)

$ErrorActionPreference = 'Stop'

# GameServer의 ConfigManager, Setup-Database.ps1과 같은 키를 읽음
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
    foreach ($key in 'Server', 'Port', 'Database', 'UID', 'PWD')
    {
        if (-not $db.ContainsKey($key)) { throw "config.ini [Database] is missing '$key'" }
    }
    return $db
}

try
{
    $db = Read-DbConfig $ConfigPath
    $connectionString = "Server=$($db['Server']);Port=$($db['Port']);Database=$($db['Database']);User ID=$($db['UID']);Password=$($db['PWD']);"

    # 이미 있는 JWT 비밀키는 유지 (목록은 이 프로세스 안에서만 보고 출력하지 않음)
    $listing = & dotnet user-secrets list --project $PSScriptRoot 2>$null
    $existing = @($listing | Where-Object { $_ -like 'Auth:JwtSecret = *' })
    if ($existing.Count -gt 0)
    {
        $jwtSecret = ($existing[0] -split ' = ', 2)[1]
        $state = 'kept'
    }
    else
    {
        $bytes = New-Object byte[] 32
        [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
        $jwtSecret = [Convert]::ToBase64String($bytes)
        $state = 'created'
    }

    # 명령줄 인자로 넘기면 다른 프로세스 목록에 비밀번호가 보일 수 있어서, JSON을 표준 입력으로 넘김
    $json = @{ 'ConnectionStrings:Mmo' = $connectionString; 'Auth:JwtSecret' = $jwtSecret } | ConvertTo-Json -Compress
    $json | & dotnet user-secrets set --project $PSScriptRoot | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "dotnet user-secrets set failed (exit $LASTEXITCODE)" }

    Write-Host "Saved ConnectionStrings:Mmo (database $($db['Database'])) and Auth:JwtSecret ($state)"
    exit 0
}
catch
{
    Write-Host "[Setup-Secrets] $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
