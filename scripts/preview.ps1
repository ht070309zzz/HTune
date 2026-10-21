param(
    [string]$OutDirectory='',
    [switch]$Software,
    [switch]$Bench,
    [string]$Only='',
    [ValidateRange(1,32)][int]$Jobs=2
)
# Development previews only: builds the MusicStagePreview tool (excluded from
# the default build) and renders synthetic scenes to PNG. Opens no window,
# reads no user data and never starts or touches MusicStage or NetEase.
$ErrorActionPreference='Stop'
$env:VSLANG='1033'
$projectRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'native-build-support.ps1')
$previewLock=$null
try {
    $previewLock=Open-NativeWorkspaceLock $projectRoot
    $cmakeSource=Get-Content -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt') -Raw
    if($cmakeSource -notmatch 'project\(MusicStageNative\s+VERSION\s+(\d+\.\d+\.\d+)'){throw 'Project version not found'}
    $previewVersion=$Matches[1]
    $vswherePath=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if(!(Test-Path -LiteralPath $vswherePath)){throw 'Visual Studio discovery tool not found'}
    $installation=@(& $vswherePath -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    $installation=if($installation.Count){[string]$installation[0]}else{''}
    $installation=$installation.Trim()
    if([string]::IsNullOrWhiteSpace($installation)){throw 'MSVC C++ x64 toolchain not found'}
    $cmakeExe=Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    $ninjaExe=Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
    $vcvars=Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
    $buildRoot=Join-Path $projectRoot 'build'
    # Same configure arguments as build.ps1, so the shared cache is unchanged.
    $configureRoot=Join-Path $buildRoot 'msvc-release'
    $binaryRoot=Join-Path $buildRoot 'bin'
    if([string]::IsNullOrWhiteSpace($OutDirectory)){
        $OutDirectory=Join-Path $buildRoot ('preview\'+$previewVersion+'\'+[DateTime]::Now.ToString('yyyyMMdd-HHmmss'))
    }
    [IO.Directory]::CreateDirectory($OutDirectory) | Out-Null
    $unsafePaths=@($vcvars,$cmakeExe,$ninjaExe,$projectRoot,$buildRoot,$configureRoot,$binaryRoot) | Where-Object { $_ -match '["\r\n%]' }
    if($unsafePaths.Count){throw 'Invalid toolchain or project path'}
    $wrapperPath=Join-Path $buildRoot 'native-preview.cmd'
    $wrapper=@(
      '@echo off',
      'chcp 65001 >nul',
      'setlocal DisableDelayedExpansion',
      'set VSLANG=1033',
      ('call "'+$vcvars+'" >nul'),
      'if errorlevel 1 exit /b 1',
      'set VSLANG=1033',
      ('"'+$cmakeExe+'" -S "'+$projectRoot+'" -B "'+$configureRoot+'" -G Ninja "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_MAKE_PROGRAM='+$ninjaExe+'" "-DMUSIC_STAGE_OUTPUT_ROOT='+$binaryRoot+'"'),
      'if errorlevel 1 exit /b 1',
      ('"'+$cmakeExe+'" --build "'+$configureRoot+'" --target MusicStagePreview --parallel '+$Jobs),
      'exit /b %errorlevel%'
    )
    [IO.File]::WriteAllLines($wrapperPath,[string[]]$wrapper,[Text.UTF8Encoding]::new($false))
    $previewLog=Join-Path $OutDirectory 'build.log'
    $previewExit=& {
        $ErrorActionPreference='Continue'
        & $env:ComSpec /d /v:off /c $wrapperPath *> $previewLog
        $LASTEXITCODE
    }
    if($previewExit -ne 0){Get-Content -LiteralPath $previewLog | Select-Object -Last 40; throw "Preview tool build failed (exit $previewExit); log: $previewLog"}
    $tool=Join-Path $configureRoot 'preview-tool\MusicStagePreview.exe'
    if(!(Test-Path -LiteralPath $tool)){throw 'Preview tool was not produced'}
    $toolArguments=@('--out',$OutDirectory)
    if($Software){$toolArguments+='--software'}
    if($Bench){$toolArguments+='--bench'}
    if(![string]::IsNullOrWhiteSpace($Only)){$toolArguments+=@('--only',$Only)}
    & $tool @toolArguments
    if($LASTEXITCODE -ne 0){throw "Preview tool failed (exit $LASTEXITCODE)"}
    Write-Output ('Previews: '+$OutDirectory)
} finally {
    if($previewLock){$previewLock.Dispose()}
}
