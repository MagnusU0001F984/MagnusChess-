[CmdletBinding()]
param(
    [ValidateRange(0, 4096)]
    [int]$StaticBase = 176,

    [ValidateRange(0, 512)]
    [int]$StaticDepthSlope = 8,

    [ValidateRange(0, 4096)]
    [int]$ImprovingMargin = 64,

    [ValidateRange(1, 4096)]
    [int]$EvalBucket = 96,

    [ValidateRange(1, 16)]
    [int]$MinReduction = 2,

    [ValidateRange(0, 128)]
    [int]$VerificationMinDepth = 16,

    [string]$TargetName = "valerain_nmp_batch1",

    [string]$Tag = "",

    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$srcDir = Join-Path $repoRoot "src"
$logDir = Join-Path $srcDir "build\tuning\nmp-batch1"

New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$makeCommand = Get-Command "mingw32-make" -ErrorAction SilentlyContinue
if (-not $makeCommand) {
    $makeCommand = Get-Command "make" -ErrorAction SilentlyContinue
}
if (-not $makeCommand) {
    throw "Could not find mingw32-make or make in PATH."
}

$makeArgs = @(
    "-j",
    "TARGET=$TargetName",
    "SEARCH_OBS=1",
    "NMP_STATIC_BASE=$StaticBase",
    "NMP_STATIC_DEPTH_SLOPE=$StaticDepthSlope",
    "NMP_IMPROVING_MARGIN=$ImprovingMargin",
    "NMP_EVAL_BUCKET=$EvalBucket",
    "NMP_MIN_REDUCTION=$MinReduction",
    "NMP_VERIFICATION_MIN_DEPTH=$VerificationMinDepth"
)

Push-Location $srcDir
try {
    if ($Clean) {
        & $makeCommand.Source "clean"
        if ($LASTEXITCODE -ne 0) {
            throw "Clean failed with exit code $LASTEXITCODE."
        }
    }

    & $makeCommand.Source @makeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}

$engineCandidates = @(
    (Join-Path $repoRoot ("src\bin\" + $TargetName + ".exe")),
    (Join-Path $repoRoot ("src\bin\" + $TargetName))
)
$enginePath = $engineCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $enginePath) {
    throw "Could not locate the built engine binary."
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$slug = "sb${StaticBase}_sd${StaticDepthSlope}_im${ImprovingMargin}_eb${EvalBucket}_mr${MinReduction}_vd${VerificationMinDepth}"
if ($Tag) {
    $slug = "$slug-$Tag"
}
$logPath = Join-Path $logDir "$stamp-$slug.log"

Push-Location $repoRoot
try {
    "bench`nquit`n" | & $enginePath uci | Tee-Object -FilePath $logPath
    if ($LASTEXITCODE -ne 0) {
        throw "Bench failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}

$totals = [ordered]@{
    candidates = [int64]0
    tried = [int64]0
    failhigh = [int64]0
    verify = [int64]0
    verified = [int64]0
    failed = [int64]0
}

$nodesPerSecond = [int64]0

foreach ($line in Get-Content $logPath) {
    if ($line -match "info string searchobs nmp candidates (\d+) tried (\d+) failhigh (\d+) verify (\d+) verified (\d+) failed (\d+)") {
        $totals.candidates += [int64]$matches[1]
        $totals.tried += [int64]$matches[2]
        $totals.failhigh += [int64]$matches[3]
        $totals.verify += [int64]$matches[4]
        $totals.verified += [int64]$matches[5]
        $totals.failed += [int64]$matches[6]
        continue
    }

    if ($line -match "^Nodes/second\s+:\s+(\d+)$") {
        $nodesPerSecond = [int64]$matches[1]
    }
}

$failHighRate = if ($totals.tried -gt 0) {
    [math]::Round((100.0 * $totals.failhigh) / $totals.tried, 2)
}
else {
    0.0
}

$verificationSuccessRate = if ($totals.verify -gt 0) {
    [math]::Round((100.0 * $totals.verified) / $totals.verify, 2)
}
else {
    0.0
}

Write-Host ""
Write-Host "NMP batch-1 summary"
Write-Host "  log                       : $logPath"
Write-Host "  nodes_per_second          : $nodesPerSecond"
Write-Host "  nmp_candidates            : $($totals.candidates)"
Write-Host "  nmp_tried                 : $($totals.tried)"
Write-Host "  nmp_fail_high             : $($totals.failhigh)"
Write-Host "  nmp_fail_high_rate_pct    : $failHighRate"
Write-Host "  verification_triggered    : $($totals.verify)"
Write-Host "  verification_verified     : $($totals.verified)"
Write-Host "  verification_failed       : $($totals.failed)"
Write-Host "  verification_success_pct  : $verificationSuccessRate"
