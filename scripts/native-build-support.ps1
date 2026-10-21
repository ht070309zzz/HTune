# Shared by the native build and final-delivery scripts. These functions only
# inspect project/build files; they never start the application or acceptance.
function Open-NativeWorkspaceLock {
    param([Parameter(Mandatory=$true)][string]$ProjectRoot)
    $directory=Join-Path $ProjectRoot 'build'
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    try {
        return [IO.File]::Open((Join-Path $directory 'native-work.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    } catch [IO.IOException] {
        $code=$_.Exception.HResult -band 0xffff
        if($code -eq 32 -or $code -eq 33){throw 'Another native build or package operation holds this project lock. Retry after it finishes; do not remove the lock file to bypass it.'}
        throw
    }
}

function Get-NativeHash {
    param([Parameter(Mandatory=$true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
}

function Get-NativeRowsDigest {
    param([Parameter(Mandatory=$true)][object[]]$Rows)
    $text=[Text.StringBuilder]::new()
    foreach($row in $Rows) {
        [void]$text.Append([string]$row.path).Append("`t").Append(([long]$row.length).ToString([Globalization.CultureInfo]::InvariantCulture)).Append("`t").Append([string]$row.sha256).Append("`n")
    }
    $sha=[Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text.ToString())))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Get-NativeSourceSnapshot {
    param([Parameter(Mandatory=$true)][string]$ProjectRoot)
    $root=[IO.Path]::GetFullPath($ProjectRoot).TrimEnd([char[]]'\/')
    $prefix=$root+[IO.Path]::DirectorySeparatorChar
    $paths=[Collections.Generic.List[string]]::new()
    $paths.Add('CMakeLists.txt')
    foreach($notice in @('LICENSE','THIRD-PARTY-NOTICES.md')){
        if(Test-Path -LiteralPath (Join-Path $root $notice) -PathType Leaf){$paths.Add($notice)}
    }
    foreach($directory in @('src','include','assets','scripts')) {
        $folder=Join-Path $root $directory
        if(!(Test-Path -LiteralPath $folder -PathType Container)){throw "Missing native source directory: $directory"}
        # Do not silently omit linked material from a supposedly complete list.
        if((Get-Item -LiteralPath $folder -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Linked native source directory is not supported: $directory"}
        foreach($item in Get-ChildItem -LiteralPath $folder -Force -Recurse) {
            if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Linked source material cannot be represented by this native build receipt.'}
            if($item.PSIsContainer){continue}
            if(!$item.FullName.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Native source path escaped the project root.'}
            $relative=$item.FullName.Substring($prefix.Length).Replace('\','/')
            if($relative.IndexOfAny([char[]]"`r`n`t") -ge 0){throw 'Native source names cannot contain receipt delimiters.'}
            $paths.Add($relative)
        }
    }
    [string[]]$orderedPaths=$paths.ToArray()
    [Array]::Sort($orderedPaths,[StringComparer]::Ordinal)
    $rows=@(foreach($relative in $orderedPaths) {
        $file=Get-Item -LiteralPath (Join-Path $root $relative) -Force -ErrorAction Stop
        if($file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw "Native input is not a regular file: $relative"}
        [pscustomobject][ordered]@{path=$relative;length=[long]$file.Length;sha256=Get-NativeHash $file.FullName}
    })
    return [pscustomobject][ordered]@{sha256=Get-NativeRowsDigest $rows;files=$rows}
}

function Assert-NativeSourceSnapshot {
    param([Parameter(Mandatory=$true)]$Expected,[Parameter(Mandatory=$true)]$Actual)
    $before=@($Expected.files);$after=@($Actual.files)
    if(!$before.Count -or $before.Count -ne $after.Count -or $Expected.sha256 -cne $Actual.sha256){throw 'Native source inputs differ from the successful build. Build the current source before packaging.'}
    for($index=0;$index -lt $before.Count;$index++) {
        if($before[$index].path -cne $after[$index].path -or $before[$index].length -ne $after[$index].length -or $before[$index].sha256 -cne $after[$index].sha256){throw 'Native source input records differ from the successful build.'}
    }
}

function Write-NativeJsonAtomic {
    param([Parameter(Mandatory=$true)][string]$Path,[Parameter(Mandatory=$true)]$Value)
    $temporary=$Path+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
    $encoded=[Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText($temporary,($Value | ConvertTo-Json -Depth 12)+"`n",$encoded)
    if([IO.File]::Exists($Path)){
        # PowerShell can bind $null as an empty backup string for this .NET
        # overload. A unique real backup also preserves the previous record.
        $backup=$Path+'.'+[Guid]::NewGuid().ToString('N')+'.previous.json'
        [IO.File]::Replace($temporary,$Path,$backup)
    }else{[IO.File]::Move($temporary,$Path)}
}

function Read-NativeBuildReceipt {
    param([Parameter(Mandatory=$true)][string]$Path)
    $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try {
        if($stream.Length -lt 1 -or $stream.Length -gt 4MB){throw 'Native build receipt is empty or exceeds 4 MiB.'}
        $reader=[IO.StreamReader]::new($stream,[Text.UTF8Encoding]::new($false,$true),$true)
        try { return ($reader.ReadToEnd() | ConvertFrom-Json -ErrorAction Stop) }
        finally { $reader.Dispose() }
    } finally { $stream.Dispose() }
}

function Get-NativeBinaryRecord {
    param([Parameter(Mandatory=$true)][string]$Path,[Parameter(Mandatory=$true)][string]$Version)
    $file=Get-Item -LiteralPath $Path -ErrorAction Stop
    $stream=[IO.File]::Open($file.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try {
        if($stream.Length -lt 64){throw 'Native executable is too small to contain a PE header.'}
        $reader=[IO.BinaryReader]::new($stream,[Text.Encoding]::ASCII,$true)
        try {
            if($reader.ReadUInt16() -ne 0x5a4d){throw 'Native executable does not have an MZ header.'}
            $stream.Position=0x3c;$peOffset=$reader.ReadInt32()
            if($peOffset -lt 64 -or $peOffset -gt ($stream.Length-6)){throw 'Native executable PE header offset is invalid.'}
            $stream.Position=$peOffset
            if($reader.ReadUInt32() -ne 0x00004550){throw 'Native executable does not have a PE signature.'}
            $machine=$reader.ReadUInt16()
            if($machine -ne 0x8664){throw ('Native executable is not AMD64 (PE Machine 0x{0:x4}).' -f $machine)}
        } finally {$reader.Dispose()}
    } finally {$stream.Dispose()}
    $info=$file.VersionInfo
    $product='{0}.{1}.{2}' -f $info.ProductMajorPart,$info.ProductMinorPart,$info.ProductBuildPart
    $resource='{0}.{1}.{2}' -f $info.FileMajorPart,$info.FileMinorPart,$info.FileBuildPart
    if($product -ne $Version -or $resource -ne $Version){throw 'Native executable file/product version resources differ from the selected version.'}
    return [pscustomobject][ordered]@{architecture='x64';peMachine='0x8664';length=[long]$file.Length;sha256=Get-NativeHash $Path;fileVersion=$info.FileVersion;productVersion=$info.ProductVersion;lastWriteUtc=$file.LastWriteTimeUtc.ToString('o')}
}

function Assert-NativeBinaryRecord {
    param([Parameter(Mandatory=$true)]$Expected,[Parameter(Mandatory=$true)]$Actual)
    if($Expected.architecture -cne 'x64' -or $Expected.peMachine -cne '0x8664' -or
       $Actual.architecture -cne 'x64' -or $Actual.peMachine -cne '0x8664' -or
       $Expected.sha256 -cne $Actual.sha256 -or $Expected.length -ne $Actual.length -or
       $Expected.fileVersion -cne $Actual.fileVersion -or $Expected.productVersion -cne $Actual.productVersion){
        throw 'Native executable architecture or content differs from the successful build receipt.'
    }
}

function Get-NativeDeliveryRows {
    param([Parameter(Mandatory=$true)][string]$DeliveryRoot)
    $root=[IO.Path]::GetFullPath($DeliveryRoot).TrimEnd([char[]]'\/')
    $prefix=$root+[IO.Path]::DirectorySeparatorChar
    $rows=@(Get-ChildItem -LiteralPath $root -File -Recurse -Force | ForEach-Object {
        $relative=$_.FullName.Substring($prefix.Length).Replace('\','/')
        if($relative -ne 'files.sha256.json') {
            [pscustomobject][ordered]@{path=$relative;length=[long]$_.Length;sha256=Get-NativeHash $_.FullName}
        }
    })
    return @($rows | Sort-Object -Property path -CaseSensitive)
}

function Assert-NativeArchive {
    param([Parameter(Mandatory=$true)][string]$ZipPath,[Parameter(Mandatory=$true)][string]$RootName,[Parameter(Mandatory=$true)][object[]]$Rows)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $expected=[Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
    foreach($row in $Rows){$expected.Add([string]$row.path,$row)}
    $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $prefix=$RootName+'/'
    $archive=[IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        foreach($entry in $archive.Entries) {
            $name=$entry.FullName.Replace('\','/')
            if($name.EndsWith('/')){continue}
            if(!$name.StartsWith($prefix,[StringComparison]::Ordinal)){throw 'Archive contains an unexpected root.'}
            $relative=$name.Substring($prefix.Length)
            if(!$expected.ContainsKey($relative) -or !$seen.Add($relative)){throw 'Archive contains an unexpected or duplicate file.'}
            $row=$expected[$relative]
            if($entry.Length -ne $row.length){throw "Archive file length differs: $relative"}
            $stream=$entry.Open();$sha=[Security.Cryptography.SHA256]::Create()
            try { $hash=([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','').ToLowerInvariant() }
            finally {$sha.Dispose();$stream.Dispose()}
            if($hash -cne $row.sha256){throw "Archive file content differs: $relative"}
        }
        if($seen.Count -ne $expected.Count){throw 'Archive omitted one or more delivery files.'}
    } finally {$archive.Dispose()}
}
