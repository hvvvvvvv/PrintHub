<#
.SYNOPSIS
    Configures and builds PrintHub with the installed Windows MSVC toolchain.

.EXAMPLE
    .\scripts\build.ps1

.EXAMPLE
    .\scripts\build.ps1 -Architecture x64 -Configuration Debug
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86')]
    [string]$Architecture = 'x64',

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [switch]$CleanFirst,

    [switch]$Help
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-Info {
    param([string]$Message)
    Write-Host "[PrintHub] $Message"
}

function Stop-Build {
    param([string]$Message)
    throw $Message
}

function Show-Usage {
    @'
PrintHub Windows build script

Usage:
  scripts\build.cmd [options]
  powershell -File scripts\build.ps1 [options]

Options:
  -Architecture <x64|x86>         Target architecture. Default: x64
  -Configuration <Debug|Release>  Build configuration. Default: Release
  -CleanFirst                     Rebuild generated targets without stale objects.
  -Help                           Show this help message.

Examples:
  scripts\build.cmd
  scripts\build.cmd -Architecture x64 -Configuration Debug
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Architecture x86 -Configuration Release

Tool discovery order:
  1. VCPKG_ROOT, when it points to a usable vcpkg installation
  2. vcpkg bundled with the latest Visual Studio / Build Tools installation
  3. tools\vcpkg in this repository

If vcpkg is unavailable, the script clones and bootstraps it into tools\vcpkg.
The first dependency restore requires Git and network access. Visual Studio 2022
or Build Tools 2022 with the MSVC x86/x64 toolset is required and is not installed automatically.

Output:
  out\publish\<architecture>-<configuration>\PrintHub.exe
'@ | Write-Host
}

if ($Help) {
    Show-Usage
    exit 0
}

function Get-FirstExistingPath {
    param([string[]]$Candidates)

    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    return $null
}

function Get-ConfigurePresetBinaryDir {
    param(
        [object[]]$ConfigurePresets,
        [string]$PresetName,
        [string[]]$InheritanceChain = @()
    )

    if ($InheritanceChain -ccontains $PresetName) {
        Stop-Build "CMake configure preset 继承存在循环：$($InheritanceChain -join ' -> ') -> $PresetName"
    }

    $configurePreset = $ConfigurePresets | Where-Object {
        $null -ne $_ -and $null -ne $_.PSObject.Properties['name'] -and $_.name -ceq $PresetName
    } | Select-Object -First 1
    if ($null -eq $configurePreset) {
        Stop-Build "CMakePresets.json 中未找到 configure preset：$PresetName"
    }

    $binaryDirProperty = $configurePreset.PSObject.Properties['binaryDir']
    if ($null -ne $binaryDirProperty -and -not [string]::IsNullOrWhiteSpace([string]$binaryDirProperty.Value)) {
        return [string]$binaryDirProperty.Value
    }

    $inheritsProperty = $configurePreset.PSObject.Properties['inherits']
    if ($null -ne $inheritsProperty) {
        # CMake gives earlier parents precedence when multiple presets are inherited.
        foreach ($parentName in @($inheritsProperty.Value)) {
            $binaryDir = Get-ConfigurePresetBinaryDir -ConfigurePresets $ConfigurePresets `
                -PresetName $parentName -InheritanceChain ($InheritanceChain + $PresetName)
            if (-not [string]::IsNullOrWhiteSpace($binaryDir)) {
                return $binaryDir
            }
        }
    }

    return $null
}

function Get-PresetBuildDirectory {
    param(
        [string]$RepositoryRoot,
        [string]$PresetName
    )

    $presetsPath = Join-Path $RepositoryRoot 'CMakePresets.json'
    $presets = Get-Content -LiteralPath $presetsPath -Encoding UTF8 -Raw | ConvertFrom-Json
    if ($null -eq $presets -or $null -eq $presets.PSObject.Properties['configurePresets']) {
        Stop-Build 'CMakePresets.json 中未找到 configurePresets。'
    }

    $buildDirectory = Get-ConfigurePresetBinaryDir -ConfigurePresets @($presets.configurePresets) -PresetName $PresetName
    if ([string]::IsNullOrWhiteSpace($buildDirectory)) {
        Stop-Build "CMake configure preset 未定义或继承 binaryDir：$PresetName"
    }

    $buildDirectory = $buildDirectory.Replace('${sourceDir}', $RepositoryRoot).Replace('${presetName}', $PresetName)
    if (-not [IO.Path]::IsPathRooted($buildDirectory)) {
        $buildDirectory = Join-Path $RepositoryRoot $buildDirectory
    }
    return [IO.Path]::GetFullPath($buildDirectory)
}

function Get-VisualStudioInstallation {
    $vswhereCommand = Get-Command 'vswhere.exe' -CommandType Application -ErrorAction SilentlyContinue
    $vswherePath = $null
    if ($vswhereCommand) {
        $vswherePath = $vswhereCommand.Source
    }
    else {
        $vswhereCandidates = @()
        if (${env:ProgramFiles(x86)}) {
            $vswhereCandidates += Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        }
        if ($env:ProgramFiles) {
            $vswhereCandidates += Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe'
        }
        $vswherePath = Get-FirstExistingPath $vswhereCandidates
    }

    if (-not $vswherePath) {
        Stop-Build '未找到 vswhere.exe。请安装 Visual Studio 2022 或 Build Tools 2022，并选择“使用 C++ 的桌面开发”工作负载。'
    }

    $installationPath = @(
        & $vswherePath -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    ) | Select-Object -First 1

    if (-not $installationPath) {
        Stop-Build '未找到包含 MSVC x86/x64 工具集的 Visual Studio 2022 或 Build Tools 2022 安装。'
    }

    $devCmdPath = Join-Path $installationPath 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $devCmdPath -PathType Leaf)) {
        Stop-Build "找到了 Visual Studio，但缺少开发环境脚本：$devCmdPath"
    }

    return [pscustomobject]@{
        InstallationPath = (Resolve-Path -LiteralPath $installationPath).Path
        DevCmdPath       = (Resolve-Path -LiteralPath $devCmdPath).Path
    }
}

function Get-CmakePath {
    param([string]$VisualStudioPath)

    $pathCommand = Get-Command 'cmake.exe' -CommandType Application -ErrorAction SilentlyContinue
    $candidates = @()
    if ($pathCommand) {
        $candidates += $pathCommand.Source
    }
    $candidates += Join-Path $VisualStudioPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'

    foreach ($candidate in $candidates) {
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            continue
        }

        $versionOutput = (& $candidate --version 2>&1) -join "`n"
        if ($LASTEXITCODE -ne 0 -or $versionOutput -notmatch '(\d+\.\d+\.\d+)') {
            continue
        }

        if ([version]$Matches[1] -ge [version]'3.21.0') {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    Stop-Build '未找到 CMake 3.21 或更高版本。请安装 Visual Studio 的 CMake 工具组件，或将兼容版本的 cmake.exe 加入 PATH。'
}

function Get-NinjaPath {
    param([string]$VisualStudioPath)

    $pathCommand = Get-Command 'ninja.exe' -CommandType Application -ErrorAction SilentlyContinue
    $candidates = @()
    if ($pathCommand) {
        $candidates += $pathCommand.Source
    }
    $candidates += Join-Path $VisualStudioPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'

    $ninjaPath = Get-FirstExistingPath $candidates
    if (-not $ninjaPath) {
        Stop-Build '未找到 Ninja。请安装 Visual Studio 的 CMake 工具组件，或将 ninja.exe 加入 PATH。'
    }

    return $ninjaPath
}

function Test-VcpkgRoot {
    param([string]$Candidate)

    if (-not $Candidate -or -not (Test-Path -LiteralPath $Candidate -PathType Container)) {
        return $null
    }

    $root = (Resolve-Path -LiteralPath $Candidate).Path
    $executablePath = Join-Path $root 'vcpkg.exe'
    $toolchainPath = Join-Path $root 'scripts\buildsystems\vcpkg.cmake'
    if ((Test-Path -LiteralPath $executablePath -PathType Leaf) -and (Test-Path -LiteralPath $toolchainPath -PathType Leaf)) {
        return $root
    }

    return $null
}

function Get-VcpkgRoot {
    param(
        [string]$RepositoryRoot,
        [string]$VisualStudioPath
    )

    $candidates = @(
        $env:VCPKG_ROOT,
        (Join-Path $VisualStudioPath 'VC\vcpkg'),
        (Join-Path $RepositoryRoot 'tools\vcpkg')
    )

    foreach ($candidate in $candidates) {
        $validRoot = Test-VcpkgRoot $candidate
        if ($validRoot) {
            return $validRoot
        }
    }

    $gitCommand = Get-Command 'git.exe' -CommandType Application -ErrorAction SilentlyContinue
    if (-not $gitCommand) {
        Stop-Build '未找到可用的 vcpkg，且未找到 Git，无法自动引导 vcpkg。请安装 Git 或设置有效的 VCPKG_ROOT。'
    }

    $localRoot = Join-Path $RepositoryRoot 'tools\vcpkg'
    if (Test-Path -LiteralPath $localRoot) {
        Stop-Build "项目本地 vcpkg 目录不完整：$localRoot。请删除该目录后重试，或设置有效的 VCPKG_ROOT。"
    }

    Write-Info '未找到 vcpkg，正在下载并引导项目本地 vcpkg...'
    New-Item -ItemType Directory -Path (Split-Path -Parent $localRoot) -Force | Out-Null
    & $gitCommand.Source clone --depth 1 https://github.com/microsoft/vcpkg.git $localRoot
    if ($LASTEXITCODE -ne 0) {
        Stop-Build '下载 vcpkg 失败。请检查 Git、网络连接或设置有效的 VCPKG_ROOT。'
    }

    $bootstrapPath = Join-Path $localRoot 'bootstrap-vcpkg.bat'
    & $bootstrapPath -disableMetrics
    if ($LASTEXITCODE -ne 0) {
        Stop-Build 'vcpkg 引导失败。请检查网络、代理和 Windows 开发工具链。'
    }

    $validRoot = Test-VcpkgRoot $localRoot
    if (-not $validRoot) {
        Stop-Build 'vcpkg 引导完成后仍未找到 vcpkg.exe 或 CMake 工具链文件。'
    }

    return $validRoot
}

try {
    if ($Architecture -eq 'x64' -and -not [Environment]::Is64BitOperatingSystem) {
        Stop-Build '当前操作系统为 32 位，无法构建 x64 二进制。请改用 -Architecture x86。'
    }

    $repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
    $preset = '{0}-{1}' -f $Architecture, $Configuration.ToLowerInvariant()
    $buildDirectory = Get-PresetBuildDirectory -RepositoryRoot $repositoryRoot -PresetName $preset
    $serviceBinaryPath = Join-Path $buildDirectory 'src\PrintHub.exe'
    $publishDirectory = Join-Path $repositoryRoot (Join-Path 'out\publish' $preset)

    $visualStudio = Get-VisualStudioInstallation
    $cmakePath = Get-CmakePath $visualStudio.InstallationPath
    $ninjaPath = Get-NinjaPath $visualStudio.InstallationPath
    $vcpkgRoot = Get-VcpkgRoot $repositoryRoot $visualStudio.InstallationPath
    $hostArchitecture = if ([Environment]::Is64BitOperatingSystem) { 'x64' } else { 'x86' }

    Write-Info "Preset: $preset"
    Write-Info "Visual Studio: $($visualStudio.InstallationPath)"
    Write-Info "CMake: $cmakePath"
    Write-Info "vcpkg: $vcpkgRoot"

    $savedVcpkgRoot = $env:VCPKG_ROOT
    $savedPath = $env:PATH
    $savedLocalAppData = $env:LOCALAPPDATA
    $savedVsLang = $env:VSLANG
    try {
        $env:VCPKG_ROOT = $vcpkgRoot
        # Prefer English when installed, but a Chinese-only compiler falls
        # back silently if 1033 is requested. Select a resource that actually
        # exists so CMake's dependency prefix matches the compiler output.
        $compilerVersionPath = Join-Path $visualStudio.InstallationPath 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
        $compilerVersion = (Get-Content -LiteralPath $compilerVersionPath -Raw).Trim()
        $compilerUiDirectory = Join-Path $visualStudio.InstallationPath ("VC\Tools\MSVC\$compilerVersion\bin\Host$hostArchitecture\$Architecture")
        if (Test-Path -LiteralPath (Join-Path $compilerUiDirectory '1033\clui.dll')) { $env:VSLANG = '1033' }
        elseif (Test-Path -LiteralPath (Join-Path $compilerUiDirectory '2052\clui.dll')) { $env:VSLANG = '2052' }
        $env:PATH = (Split-Path -Parent $cmakePath) + ';' + (Split-Path -Parent $ninjaPath) + ';' + $savedPath
        # Keep vcpkg's mutable registry clone within the build output. This
        # avoids requiring write access to a user's global AppData cache.
        $env:LOCALAPPDATA = Join-Path $repositoryRoot 'out\vcpkg-localappdata'
        New-Item -ItemType Directory -Path $env:LOCALAPPDATA -Force | Out-Null

        Push-Location $repositoryRoot
        try {
            # VsDevCmd can leave PATH and Path with different values when
            # invoked from PowerShell. CMake observes Path, so normalize it
            # after activating the MSVC environment.
            $cleanArgument = if ($CleanFirst) { ' --clean-first' } else { '' }
            if ($CleanFirst -and -not ([IO.Path]::GetFullPath($buildDirectory).StartsWith(
                (Join-Path $repositoryRoot 'out\build\'), [StringComparison]::OrdinalIgnoreCase))) {
                Stop-Build 'Refusing to clean a build directory outside this workspace.'
            }
            $commandLine = 'call "{0}" -arch={1} -host_arch={2} && set "Path=!PATH!" && chcp 65001 >nul && "{3}" --preset {4} && "{3}" --build "{5}"{6}' -f `
                $visualStudio.DevCmdPath, $Architecture, $hostArchitecture, $cmakePath, $preset, $buildDirectory, $cleanArgument
            & $env:ComSpec /v:on /d /c $commandLine
            if ($LASTEXITCODE -ne 0) {
                Stop-Build "CMake 构建失败，退出代码：$LASTEXITCODE"
            }
        }
        finally {
            Pop-Location
        }
    }
    finally {
        $env:VCPKG_ROOT = $savedVcpkgRoot
        $env:PATH = $savedPath
        $env:LOCALAPPDATA = $savedLocalAppData
        $env:VSLANG = $savedVsLang
    }

    if (-not (Test-Path -LiteralPath $serviceBinaryPath -PathType Leaf)) {
        Stop-Build "构建命令已完成，但未找到输出二进制：$serviceBinaryPath"
    }

    New-Item -ItemType Directory -Path $publishDirectory -Force | Out-Null
    Copy-Item -LiteralPath $serviceBinaryPath -Destination (Join-Path $publishDirectory 'PrintHub.exe') -Force

    if ($configuration -eq 'Release') {
        $servicePdbPath = Join-Path $buildDirectory 'src\PrintHub.pdb'
        if (-not (Test-Path -LiteralPath $servicePdbPath -PathType Leaf)) {
            throw "未生成 PrintHub PDB：$servicePdbPath"
        }
        $symbolDirectory = Join-Path $repositoryRoot (Join-Path 'out\symbols' $preset)
        New-Item -ItemType Directory -Path $symbolDirectory -Force | Out-Null
        Copy-Item -LiteralPath $servicePdbPath -Destination (Join-Path $symbolDirectory 'PrintHub.pdb') -Force
    }

    Write-Info "构建成功，发布目录：$publishDirectory"
}
catch {
    Write-Error "[PrintHub] $($_.Exception.Message)"
    exit 1
}
