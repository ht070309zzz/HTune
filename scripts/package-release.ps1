param([string]$IsccPath = '')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'native-build-support.ps1')
$packageLock = $null
try {
    $packageLock = Open-NativeWorkspaceLock $projectRoot
    $cmake = Get-Content -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt') -Raw
    if ($cmake -notmatch 'project\(MusicStageNative\s+VERSION\s+(\d+\.\d+\.\d+)') { throw 'Project version not found.' }
    $version = $Matches[1]
    $receipt = Read-NativeBuildReceipt (Join-Path $projectRoot "build/provenance/release/$version/latest.json")
    if ($receipt.status -ne 'success' -or $receipt.configuration -ne 'Release' -or $receipt.version -ne $version) {
        throw 'A successful Release build of the current version is required.'
    }
    $inputs = Get-NativeSourceSnapshot $projectRoot
    Assert-NativeSourceSnapshot $receipt.inputs $inputs
    $binaryPath = Join-Path $projectRoot "build/bin/$version/HTune.exe"
    $binary = Get-NativeBinaryRecord $binaryPath $version
    Assert-NativeBinaryRecord $receipt.binary.content $binary

    if (!$IsccPath) {
        $candidates = @(
            (Join-Path $env:LOCALAPPDATA 'Programs/Inno Setup 6/ISCC.exe'),
            (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6/ISCC.exe'),
            (Join-Path $env:ProgramFiles 'Inno Setup 6/ISCC.exe')
        )
        $IsccPath = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    }
    if (!$IsccPath -or !(Test-Path -LiteralPath $IsccPath -PathType Leaf)) {
        throw 'Install Inno Setup 6.3 or newer from jrsoftware.org, or pass -IsccPath.'
    }
    $IsccPath = (Resolve-Path -LiteralPath $IsccPath).Path

    # A unique new directory prevents stale files and preserves prior artifacts.
    $runId = [Guid]::NewGuid().ToString('N')
    $runRoot = Join-Path $projectRoot "dist/releases/$version/$runId"
    $releaseDir = Join-Path $runRoot 'assets'
    $payloadName = "HTune-$version-windows-x64"
    $payload = Join-Path $runRoot $payloadName
    [void][IO.Directory]::CreateDirectory($releaseDir)
    [void][IO.Directory]::CreateDirectory($payload)
    Copy-Item -LiteralPath $binaryPath -Destination (Join-Path $payload 'HTune.exe')
    $publicFiles = @(
        'LICENSE', 'THIRD-PARTY-NOTICES.md', 'README.md',
        'assets/htune.ico', 'assets/htune-icon-source.png',
        'include/third_party/qqmusicdecoder/LICENSE',
        'include/third_party/miniz/LICENSE',
        'include/third_party/qqidentity/LICENSE',
        'include/third_party/kugou-layout/LICENSE'
    )
    foreach ($relative in $publicFiles) {
        $source = Join-Path $projectRoot $relative
        $item = Get-Item -LiteralPath $source -ErrorAction Stop
        if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Not a regular release file: $relative" }
        $destination = Join-Path $payload $relative
        [void][IO.Directory]::CreateDirectory((Split-Path -Parent $destination))
        Copy-Item -LiteralPath $source -Destination $destination
    }
    # Public metadata deliberately excludes machine paths, build logs and user data.
    Write-NativeJsonAtomic (Join-Path $payload 'release.json') ([ordered]@{
        product='HTune'; version=$version; architecture='x64'; minimumWindowsBuild=22000
        source='https://github.com/ht070309zzz/HTune'; sourceInputsSha256=$inputs.sha256
        executableSha256=$binary.sha256; runtimeTestedDuringPackaging=$false
    })
    $rows = @(Get-NativeDeliveryRows $payload)
    Assert-NativeBinaryRecord $binary (Get-NativeBinaryRecord (Join-Path $payload 'HTune.exe') $version)

    $zip = Join-Path $releaseDir ($payloadName + '.zip')
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($payload, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
    Assert-NativeArchive $zip $payloadName $rows

    $compilerLog = Join-Path $runRoot 'installer-compile.log'
    & $IsccPath "/DAppVersion=$version" "/DPayloadDir=$payload" "/DReleaseDir=$releaseDir" (Join-Path $PSScriptRoot 'htune-installer.iss') *> $compilerLog
    if ($LASTEXITCODE -ne 0) { throw "Installer compilation failed; see $compilerLog" }
    $installer = Join-Path $releaseDir "HTune-Setup-$version-x64.exe"
    $installerInfo = (Get-Item -LiteralPath $installer).VersionInfo
    # Inno Setup pads its version strings; compare trimmed text and numeric parts.
    $installerVersion = '{0}.{1}.{2}.{3}' -f $installerInfo.ProductMajorPart, $installerInfo.ProductMinorPart, $installerInfo.ProductBuildPart, $installerInfo.ProductPrivatePart
    if ($installerInfo.ProductName.Trim() -ne 'HTune' -or $installerInfo.ProductVersion.Trim() -ne "$version.0" -or $installerVersion -ne "$version.0") { throw 'Unexpected installer version resource.' }
    $afterRows = @(Get-NativeDeliveryRows $payload)
    if ((Get-NativeRowsDigest $rows) -cne (Get-NativeRowsDigest $afterRows)) { throw 'Payload changed during packaging.' }
    Assert-NativeSourceSnapshot $inputs (Get-NativeSourceSnapshot $projectRoot)
    Assert-NativeBinaryRecord $binary (Get-NativeBinaryRecord $binaryPath $version)
    $assets = @(foreach ($path in @($installer, $zip)) {
        $item = Get-Item -LiteralPath $path
        [pscustomobject][ordered]@{name=$item.Name; length=[long]$item.Length; sha256=Get-NativeHash $path}
    })
    $checksums = ($assets | ForEach-Object { $_.sha256 + '  ' + $_.name }) -join "`r`n"
    [IO.File]::WriteAllText((Join-Path $releaseDir 'SHA256SUMS.txt'), $checksums + "`r`n", [Text.UTF8Encoding]::new($false))
    Write-NativeJsonAtomic (Join-Path $runRoot 'package-receipt.json') ([ordered]@{
        schema=1; status='success'; version=$version; completedUtc=[DateTime]::UtcNow.ToString('o')
        buildRunId=$receipt.runId; sourceInputsSha256=$inputs.sha256; payload=$rows; assets=$assets
        installerCompilerVersion=(Get-Item -LiteralPath $IsccPath).VersionInfo.FileVersion
        installerCompilerSha256=Get-NativeHash $IsccPath; installerCompileLogSha256=Get-NativeHash $compilerLog
        runtimeExecuted=$false; installationTested=$false; uninstallationTested=$false
    })
    Get-ChildItem -LiteralPath $releaseDir | Select-Object Name, Length
    Write-Output ("Release assets: " + $releaseDir)
} catch {
    Write-Error -ErrorRecord $_ -ErrorAction Continue
    exit 1
} finally {
    if ($packageLock) { $packageLock.Dispose() }
}
