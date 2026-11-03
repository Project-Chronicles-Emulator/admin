$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::InputEncoding = [Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Text.UTF8Encoding]::new($false)

# Invoked only over the operator's key-authenticated SSH session. The admin API
# remains loopback-only, and its token never leaves the VPS or enters a command line.
#
# The request is read with a deadline. When the operator console abandons a request (its
# 45 s timeout terminates the local ssh.exe), Windows OpenSSH does not always deliver
# end-of-input or stop this shell. An unbounded ReadToEnd then blocks forever; hundreds of
# such shells exhausted the live host's memory and stopped PostgreSQL on 2026-10-30.
$requestReadTimeout = [TimeSpan]::FromSeconds(30)
# Console.In.ReadToEndAsync runs synchronously in Windows PowerShell, so read the raw input
# stream, whose asynchronous read runs on a worker thread and can be abandoned.
$requestReader = [IO.StreamReader]::new([Console]::OpenStandardInput(), [Text.UTF8Encoding]::new($false))
$requestRead = $requestReader.ReadToEndAsync()
if (-not $requestRead.Wait($requestReadTimeout)) {
    # The pending read keeps a thread blocked, so end the process instead of throwing.
    [Environment]::Exit(2)
}
$inputText = $requestRead.Result
if ($inputText.Length -gt 262144) {
    throw 'The admin request is too large.'
}
$request = $inputText | ConvertFrom-Json
$path = [string]$request.path
$method = [string]$request.method
if ($method -notin @('GET', 'POST') -or
    -not $path.StartsWith('/admin/v1/') -or
    $path.Length -gt 2048 -or $path -match '[\r\n#]' -or $path.Contains('..')) {
    throw 'Unsupported admin API request.'
}

function ConvertTo-CurlConfigValue {
    param([string]$Value)

    return '"' + $Value.Replace('\', '\\').Replace('"', '\"').Replace("`r", '\r').Replace("`n", '\n') + '"'
}

$token = ([string](Get-Content -LiteralPath 'C:/Chronicles/private/admin-token.txt' -Raw)).Trim()
if ($token -notmatch '^[a-f0-9]{64}$') {
    throw 'The server admin token is not configured correctly.'
}
$configuration = @(
    'silent',
    'show-error',
    'connect-timeout = 10',
    'max-time = 20',
    'noproxy = "*"',
    ('url = ' + (ConvertTo-CurlConfigValue ('http://127.0.0.1:8080' + $path))),
    ('request = ' + (ConvertTo-CurlConfigValue $method)),
    ('header = ' + (ConvertTo-CurlConfigValue ('Authorization: Bearer ' + $token))),
    'header = "Content-Type: application/json"',
    'write-out = "\n__CHRONICLES_STATUS__%{http_code}"'
)
if ($method -eq 'POST') {
    $configuration += 'data-binary = ' + (ConvertTo-CurlConfigValue ([string]$request.body))
}

try {
    $response = [string](($configuration -join "`n") | & "$env:SystemRoot/System32/curl.exe" --config -)
    if ($LASTEXITCODE -ne 0) {
        throw "The loopback admin request failed (curl exit $LASTEXITCODE)."
    }
    $separator = $response.LastIndexOf('__CHRONICLES_STATUS__')
    if ($separator -lt 0) {
        throw 'The admin response has no HTTP status.'
    }
    $status = [int]$response.Substring($separator + '__CHRONICLES_STATUS__'.Length)
    $body = $response.Substring(0, $separator).TrimEnd()
    [ordered]@{
        status = $status
        body = [string]$body
    } | ConvertTo-Json -Compress
} finally {
    $token = $null
    $configuration = $null
}
