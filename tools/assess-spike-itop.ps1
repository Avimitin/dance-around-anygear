[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $CandidateManifest,
    [Parameter(Mandatory)]
    [string] $DatasetRoot,
    [ValidateRange(0, 10501)]
    [int] $Samples = 0,
    [switch] $SkipDatasetHash,
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
$ReportPath = Join-Path $CandidateRoot 'public-itop-evaluation.json'
$Arguments = @{
    DatasetRoot = $DatasetRoot
    ModelPath = $CandidateModel
    ExpectedSha256 = $CandidateHash
    OutputJson = $ReportPath
    Samples = $Samples
    Uv = $Uv
}
if ($SkipDatasetHash) {
    $Arguments.SkipDatasetHash = $true
}
& (Join-Path $PSScriptRoot 'evaluate-spike-itop.ps1') @Arguments
if (-not $?) {
    throw 'SPiKE public ITOP candidate evaluation did not complete.'
}
$Report = Get-Content -LiteralPath $ReportPath -Raw | ConvertFrom-Json
if ($Report.schema -ne 'dance-around-anygear.spike-itop-side.v1' -or
    ([string]$Report.candidate.model_sha256).ToUpperInvariant() -ne
        $CandidateHash) {
    throw 'ITOP report does not belong to the selected candidate.'
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
    -Name publicItop -Value ([pscustomobject]$EvaluationValue) -Force
$Candidate.acceptance.publicItopEvaluationPassed = [bool]$Report.accepted
$CandidateJson = ($Candidate | ConvertTo-Json -Depth 20) + "`n"
$TemporaryManifest = $CandidateManifest + '.partial'
[IO.File]::WriteAllText(
    $TemporaryManifest,
    $CandidateJson,
    [Text.UTF8Encoding]::new($false))
Move-Item -LiteralPath $TemporaryManifest -Destination $CandidateManifest `
    -Force
if ($Report.accepted) {
    Write-Host '[OK] Candidate retained the public ITOP baseline.'
}
else {
    Write-Warning 'Candidate regressed beyond the public ITOP gates.'
}
Write-Host "     Report: $ReportPath"
