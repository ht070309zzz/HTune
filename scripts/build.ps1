param([ValidateSet('Debug','Release')][string]$Configuration='Release',[ValidateRange(1,32)][int]$Jobs=2)
$ErrorActionPreference='Stop'
$env:VSLANG='1033'
$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'native-build-support.ps1')
$nativeLock=$null;$nativeRunRoot=$null;$nativeAttempt=$null
try {
    $nativeLock=Open-NativeWorkspaceLock $projectRoot
    # Compile script syntax without invoking the package or any test workflow.
    foreach($nativeScript in Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.ps1' -File -Recurse) {
        $nativeTokens=$null;$nativeParseErrors=$null
        [void][Management.Automation.Language.Parser]::ParseFile($nativeScript.FullName,[ref]$nativeTokens,[ref]$nativeParseErrors)
        if($nativeParseErrors.Count){throw ('Native script syntax error in '+$nativeScript.Name+': '+$nativeParseErrors[0].Message)}
    }
    $nativeInputs=Get-NativeSourceSnapshot $projectRoot
    $cmakeSource=Get-Content -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt') -Raw
    if($cmakeSource -notmatch 'project\(MusicStageNative\s+VERSION\s+(\d+\.\d+\.\d+)'){throw 'Project version not found'}
    $nativeVersion=$Matches[1]
    $buildRoot=Join-Path $projectRoot 'build'
    $nativeConfiguration=$Configuration.ToLowerInvariant()
    $nativeRunId=[Guid]::NewGuid().ToString('N')
    $nativeReceiptRoot=Join-Path $buildRoot "provenance\$nativeConfiguration\$nativeVersion"
    $nativeRunRoot=Join-Path $nativeReceiptRoot $nativeRunId
    [IO.Directory]::CreateDirectory($nativeRunRoot) | Out-Null
    $nativeLog=Join-Path $nativeRunRoot 'build.log'
    $nativeAttempt=[ordered]@{schema=1;kind='native-build';scope='compile-only';status='building';version=$nativeVersion;configuration=$Configuration;runId=$nativeRunId;startedUtc=[DateTime]::UtcNow.ToString('o');inputs=$nativeInputs;runtimeExecuted=$false;packaged=$false}
    Write-NativeJsonAtomic (Join-Path $nativeRunRoot 'attempt.json') $nativeAttempt
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswherePath)) { throw 'Visual Studio discovery tool not found' }
# Request only the ASCII-compatible path property. Parsing vswhere's complete
# localized JSON through Windows PowerShell can corrupt non-ASCII product
# descriptions before ConvertFrom-Json sees them, even though the installation
# path itself is valid.
$installation = @(& $vswherePath -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
$installation = if($installation.Count){[string]$installation[0]}else{''}
$installation = $installation.Trim()
if ([string]::IsNullOrWhiteSpace($installation)) { throw 'MSVC C++ x64 toolchain not found. Install Desktop development with C++ and a Windows SDK.' }
$cmakeExe = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
if (!(Test-Path -LiteralPath $cmakeExe)) { throw 'CMake component not found in Visual Studio' }
$configureRoot = Join-Path $buildRoot ('msvc-'+$Configuration.ToLowerInvariant())
$binaryRoot = if($Configuration -eq 'Release'){Join-Path $buildRoot 'bin'}else{Join-Path $buildRoot 'bin\debug'}
$ninjaExe = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
# Run the complete toolchain in one cmd process.  Importing the output of `set`
# into Windows PowerShell can deadlock when vcvars64 leaves helper processes
# attached to the captured pipe, while the same toolchain invocation itself is
# reliable.  Keeping vcvars, configure and build in one wrapper also prevents a
# partially imported environment from reaching CMake.
$wrapperPath = Join-Path $buildRoot ('native-build-'+$nativeConfiguration+'.cmd')
$unsafePaths = @($vcvars,$cmakeExe,$ninjaExe,$projectRoot,$buildRoot,$configureRoot,$binaryRoot) | Where-Object { $_ -match '["\r\n%]' }
if($unsafePaths.Count){throw 'Invalid toolchain or project path'}
$wrapper = @(
  '@echo off',
  'chcp 65001 >nul',
  'setlocal DisableDelayedExpansion',
  'set VSLANG=1033',
  ('call "'+$vcvars+'" >nul'),
  'if errorlevel 1 exit /b 1',
  'set VSLANG=1033',
  ('"'+$cmakeExe+'" -S "'+$projectRoot+'" -B "'+$configureRoot+'" -G Ninja "-DCMAKE_BUILD_TYPE='+$Configuration+'" "-DCMAKE_MAKE_PROGRAM='+$ninjaExe+'" "-DMUSIC_STAGE_OUTPUT_ROOT='+$binaryRoot+'"'),
  'if errorlevel 1 exit /b 1',
  ('"'+$cmakeExe+'" --build "'+$configureRoot+'" --parallel '+$Jobs),
  'exit /b %errorlevel%'
)
[IO.File]::WriteAllLines($wrapperPath,[string[]]$wrapper,[Text.UTF8Encoding]::new($false))
# Capture this invocation, rather than asking a package to guess which of
# several manually named logs belongs to the final executable. PowerShell 5.1
# native stderr records must not replace the actual process exit status.
$nativeExit=& {
    $ErrorActionPreference='Continue'
    & $env:ComSpec /d /v:off /c $wrapperPath *> $nativeLog
    $LASTEXITCODE
}
if(Test-Path -LiteralPath $nativeLog){Get-Content -LiteralPath $nativeLog}
if($nativeExit -ne 0){throw "Native configure or build failed (exit $nativeExit); log: $nativeLog"}
Assert-NativeSourceSnapshot $nativeInputs (Get-NativeSourceSnapshot $projectRoot)
$nativeBinaryPath=Join-Path $binaryRoot ($nativeVersion+'\HTune.exe')
$nativeBinary=Get-NativeBinaryRecord $nativeBinaryPath $nativeVersion
$nativeReceipt=[ordered]@{
    schema=1;kind='native-build';scope='compile-only';status='success';version=$nativeVersion;configuration=$Configuration;runId=$nativeRunId
    startedUtc=$nativeAttempt.startedUtc;completedUtc=[DateTime]::UtcNow.ToString('o');inputs=$nativeInputs
    binary=[ordered]@{path=if($Configuration -eq 'Release'){"build/bin/$nativeVersion/HTune.exe"}else{"build/bin/debug/$nativeVersion/HTune.exe"};content=$nativeBinary}
    toolchain=[ordered]@{visualStudio=$installation;cmake=$cmakeExe;ninja=$ninjaExe;cmakeCacheSha256=Get-NativeHash (Join-Path $configureRoot 'CMakeCache.txt')}
    log="provenance/$nativeConfiguration/$nativeVersion/$nativeRunId/build.log";logSha256=Get-NativeHash $nativeLog
    runtimeExecuted=$false;packaged=$false
}
$nativeAttempt.status='success';$nativeAttempt.completedUtc=$nativeReceipt.completedUtc
Write-NativeJsonAtomic (Join-Path $nativeRunRoot 'attempt.json') $nativeAttempt
Write-NativeJsonAtomic (Join-Path $nativeRunRoot 'receipt.json') $nativeReceipt
Write-NativeJsonAtomic (Join-Path $nativeReceiptRoot 'latest.json') $nativeReceipt
Get-Item -LiteralPath $nativeBinaryPath | Select-Object FullName,Length,LastWriteTime
Write-Output ('Build receipt: '+(Join-Path $nativeRunRoot 'receipt.json'))
} catch {
    $nativeFailure=$_
    if($nativeAttempt -and $nativeRunRoot){
        try {
            $nativeAttempt.status='failed';$nativeAttempt.completedUtc=[DateTime]::UtcNow.ToString('o');$nativeAttempt.error=$nativeFailure.Exception.Message
            Write-NativeJsonAtomic (Join-Path $nativeRunRoot 'attempt.json') $nativeAttempt
        } catch { Write-Warning 'Could not update the native attempt record; preserve its directory and log.' }
    }
    Write-Error -ErrorRecord $nativeFailure -ErrorAction Continue
    exit 1
} finally {
    if($nativeLock){$nativeLock.Dispose()}
}
exit 0

