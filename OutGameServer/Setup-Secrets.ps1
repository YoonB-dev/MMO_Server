# OutGameServer의 비밀값을 dotnet user-secrets에 저장한다 (저장소 밖: %APPDATA%\Microsoft\UserSecrets\<UserSecretsId>\secrets.json)
#   ConnectionStrings:Mmo : GameServer/config.ini [Database]로 만듦 (DB 접속 정보는 config.ini 한 곳에서만 관리)
#   Auth:JwtSecret        : 없으면 랜덤 32바이트로 새로 만들고, 있으면 그대로 둠 (바꾸면 이미 발급한 토큰이 전부 무효)
#   게임 서버도 같은 키로 토큰을 검증하므로, 같은 값을 config.ini [Auth] JwtSecret에도 씀 (그 섹션의 줄만 고침)
#     빌드 결과 폴더(Binary/<구성>)에 복사된 config.ini가 있으면 그것도 같이 맞춤
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

# ini 파일의 [Section] Key=Value 한 줄만 바꾸거나 추가 (다른 줄, 주석, 인코딩(UTF-8, BOM 없음), CRLF는 그대로)
function Set-IniValue
{
    param([string]$Path, [string]$Section, [string]$Key, [string]$Value)

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.AddRange([string[]](Get-Content $Path -Encoding UTF8))

    $sectionStart = -1
    $sectionEnd = $lines.Count     # 다음 섹션이 시작하는 줄 (없으면 파일 끝)
    for ($i = 0; $i -lt $lines.Count; $i++)
    {
        $line = $lines[$i].Trim()
        if ($line -eq "[$Section]") { $sectionStart = $i; continue }
        if ($sectionStart -ge 0 -and $line.StartsWith('[')) { $sectionEnd = $i; break }
    }

    if ($sectionStart -lt 0)
    {
        $lines.Add('')
        $lines.Add("[$Section]")
        $lines.Add("$Key=$Value")
    }
    else
    {
        $found = $false
        for ($i = $sectionStart + 1; $i -lt $sectionEnd; $i++)
        {
            if ($lines[$i] -match "^\s*$([regex]::Escape($Key))\s*=") { $lines[$i] = "$Key=$Value"; $found = $true; break }
        }
        if (-not $found) { $lines.Insert($sectionEnd, "$Key=$Value") }
    }

    [System.IO.File]::WriteAllText($Path, ($lines -join "`r`n") + "`r`n", (New-Object System.Text.UTF8Encoding($false)))
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

    # 게임 서버 config.ini [Auth]에도 같은 키 (+ 발급자 / 대상이 없으면 기본값)
    $configFiles = @((Resolve-Path $ConfigPath).Path)
    foreach ($configuration in 'Debug', 'Release')
    {
        $copied = Join-Path $PSScriptRoot "..\Binary\$configuration\config.ini"
        if (Test-Path $copied) { $configFiles += (Resolve-Path $copied).Path }
    }
    foreach ($file in $configFiles)
    {
        Set-IniValue -Path $file -Section 'Auth' -Key 'JwtSecret' -Value $jwtSecret
        $text = Get-Content $file -Raw -Encoding UTF8
        if ($text -notmatch '(?m)^\s*Issuer\s*=') { Set-IniValue -Path $file -Section 'Auth' -Key 'Issuer' -Value 'MMO-Auth' }
        if ($text -notmatch '(?m)^\s*Audience\s*=') { Set-IniValue -Path $file -Section 'Auth' -Key 'Audience' -Value 'MMO-Game' }
    }

    Write-Host "Saved ConnectionStrings:Mmo (database $($db['Database'])) and Auth:JwtSecret ($state)"
    Write-Host "Updated [Auth] in $($configFiles.Count) config.ini file(s)"
    exit 0
}
catch
{
    Write-Host "[Setup-Secrets] $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
