[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $CandidateManifest,
    [Parameter(Mandatory)]
    [string[]] $HeldOutManifest,
    [string] $BaselineModelPath,
    [ValidateRange(0, 10000000)]
    [int] $MaximumSamples = 0,
    [ValidateRange(0.0, 0.99)]
    [double] $MinimumRelativeImprovement = 0.02,
    [string] $Uv = 'uv'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$CandidateManifest = (Resolve-Path -LiteralPath $CandidateManifest).Path
$CandidateRoot = Split-Path -Parent $CandidateManifest
$Candidate = Get-Content -LiteralPath $CandidateManifest -Raw |
    ConvertFrom-Json
if ($Candidate.schema -ne 'dance-around-anygear.spike-candidate.v1') {
    throw "Unsupported SPiKE candidate manifest: $CandidateManifest"
}
$CandidateModel = [IO.Path]::GetFullPath(
    (Join-Path $CandidateRoot ([string]$Candidate.onnx.file)))
$CandidatePrefix = $CandidateRoot.TrimEnd('\') + '\'
if (-not $CandidateModel.StartsWith(
        $CandidatePrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Candidate ONNX path escapes its candidate directory.'
}
if (-not (Test-Path -LiteralPath $CandidateModel -PathType Leaf)) {
    throw "Candidate ONNX model is absent: $CandidateModel"
}
$CandidateHash = (Get-FileHash -LiteralPath $CandidateModel `
    -Algorithm SHA256).Hash
if ($CandidateHash -ne ([string]$Candidate.onnx.sha256).ToUpperInvariant()) {
    throw 'Candidate ONNX hash differs from its manifest.'
}
$Lock = Get-Content -LiteralPath `
    (Join-Path $RepositoryRoot 'dependency-lock.json') -Raw |
    ConvertFrom-Json
$Baseline = $Lock.spikeD4xx.export
if ([string]::IsNullOrWhiteSpace($BaselineModelPath)) {
    $BaselineModelPath = Join-Path $RepositoryRoot `
        '.deps\spike\57ddaec83dad754aed813afacab4d0591fd387b1\spike-itop-side-primary-fp16.onnx'
}
$BaselineModelPath = (Resolve-Path -LiteralPath $BaselineModelPath).Path
$ReportPath = Join-Path $CandidateRoot 'heldout-d430-evaluation.json'
& (Join-Path $PSScriptRoot 'evaluate-spike-teacher.ps1') `
    -DatasetManifest $HeldOutManifest `
    -ModelPath $CandidateModel `
    -ExpectedSha256 $CandidateHash `
    -BaselineModelPath $BaselineModelPath `
    -BaselineExpectedSha256 ([string]$Baseline.outputSha256) `
    -OutputJson $ReportPath `
    -MaximumSamples $MaximumSamples `
    -MinimumRelativeImprovement $MinimumRelativeImprovement `
    -Uv $Uv
if (-not $?) {
    throw 'SPiKE held-out candidate evaluation did not complete.'
}
$Report = Get-Content -LiteralPath $ReportPath -Raw | ConvertFrom-Json
if ($Report.schema -ne 'dance-around-anygear.spike-heldout-d430.v1' -or
    ([string]$Report.candidate.model_sha256).ToUpperInvariant() -ne
        $CandidateHash) {
    throw 'Held-out report does not belong to the selected candidate.'
}
$EvaluationValue = [ordered]@{
    report = [IO.Path]::GetFileName($ReportPath)
    reportSha256 = (Get-FileHash -LiteralPath $ReportPath `
        -Algorithm SHA256).Hash
    accepted = [bool]$Report.accepted
}
if ($null -eq $Candidate.PSObject.Properties['evaluations']) {
    $Candidate | Add-Member -MemberType NoteProperty `
        -Name evaluations -Value ([pscustomobject]@{})
}
$Candidate.evaluations | Add-Member -MemberType NoteProperty `
    -Name heldOutD430 -Value ([pscustomobject]$EvaluationValue) -Force
$Candidate.acceptance.heldOutD430EvaluationPassed = [bool]$Report.accepted
$CandidateJson = ($Candidate | ConvertTo-Json -Depth 20) + "`n"
$TemporaryManifest = $CandidateManifest + '.partial'
[IO.File]::WriteAllText(
    $TemporaryManifest,
    $CandidateJson,
    [Text.UTF8Encoding]::new($false))
Move-Item -LiteralPath $TemporaryManifest -Destination $CandidateManifest `
    -Force
if ($Report.accepted) {
    Write-Host '[OK] Candidate passed held-out D430 accuracy and latency gates.'
}
else {
    Write-Warning 'Candidate did not pass the held-out D430 acceptance gates.'
}
Write-Host "     Report: $ReportPath"
