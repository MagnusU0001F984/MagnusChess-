[CmdletBinding()]
param(
    [ValidateRange(1, 256)]
    [int]$Concurrency = 12,

    [string]$TimeControl = "8+0.08",

    [ValidateRange(1, 1024)]
    [int]$HashMb = 64,

    [string]$OpeningsFile = "C:\Users\valer\AppData\Local\Programs\Cute Chess\stc_match.pgn",

    [ValidateRange(1, 1000000)]
    [int]$Rounds = 10000
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$cuteChessDir = "C:\Users\valer\AppData\Local\Programs\Cute Chess"
$cuteChessCli = Join-Path $cuteChessDir "cutechess-cli.exe"
$engineNew = Join-Path $repoRoot "src\bin\valerain_stc_176.exe"
$engineOld = Join-Path $repoRoot "src\bin\valerain_stc_160.exe"
$evalFile = Join-Path $repoRoot "Evalfile.bin"
$stcDir = Join-Path $repoRoot "stc"

foreach ($path in @($cuteChessCli, $engineNew, $engineOld, $evalFile, $OpeningsFile)) {
    if (-not (Test-Path $path)) {
        throw "Required path not found: $path"
    }
}

New-Item -ItemType Directory -Force -Path $stcDir | Out-Null

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$pgnOut = Join-Path $stcDir "valerain_176_vs_160_${stamp}.pgn"

$args = @(
    "-engine", "name=Valerain-176", "cmd=$engineNew", "dir=$repoRoot", "proto=uci",
    "option.Hash=$HashMb", "option.UseNNUE=true", "option.EvalFile=$evalFile",
    "-engine", "name=Valerain-160", "cmd=$engineOld", "dir=$repoRoot", "proto=uci",
    "option.Hash=$HashMb", "option.UseNNUE=true", "option.EvalFile=$evalFile",
    "-each", "tc=$TimeControl", "timemargin=100"
    "-openings", "file=$OpeningsFile", "format=pgn", "order=random", "plies=8"
    "-repeat"
    "-games", "2"
    "-rounds", "$Rounds"
    "-concurrency", "$Concurrency"
    "-draw", "movenumber=40", "movecount=8", "score=8"
    "-resign", "movecount=4", "score=700", "twosided=true"
    "-recover"
    "-ratinginterval", "2"
    "-outcomeinterval", "2"
    "-sprt", "elo0=0", "elo1=5", "alpha=0.05", "beta=0.05"
    "-event", "Valerain 176 vs 160 STC"
    "-site", "D:\\CHESS\\valerain"
    "-pgnout", "$pgnOut", "min"
)

Write-Host "Launching STC..."
Write-Host "  cutechess : $cuteChessCli"
Write-Host "  new       : $engineNew"
Write-Host "  old       : $engineOld"
Write-Host "  tc        : $TimeControl"
Write-Host "  concur    : $Concurrency"
Write-Host "  openings  : $OpeningsFile"
Write-Host "  pgn       : $pgnOut"
Write-Host ""

& $cuteChessCli @args
