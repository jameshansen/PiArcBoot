# Installs PiArcBoot on Windows from PowerShell:
#
#   irm https://github.com/jameshansen/PiArcBoot/releases/latest/download/install.ps1 | iex
#
# It downloads the installer from the release it came with and runs it. The release workflow
# writes the release's tag in place of {TAG}.
& {
    $ErrorActionPreference = 'Stop'
    $ProgressPreference = 'SilentlyContinue'    # Windows PowerShell's progress bar slows downloads to a crawl
    $url = if ($env:PIARCBOOT_RELEASE) { $env:PIARCBOOT_RELEASE } else { 'https://github.com/jameshansen/PiArcBoot/releases/download/{TAG}' }
    $dir = Join-Path ([IO.Path]::GetTempPath()) "PiArcBoot-$([guid]::NewGuid())"
    New-Item -ItemType Directory $dir | Out-Null
    try {
        Write-Host 'Downloading PiArcBoot-Installer-Windows.zip'
        Invoke-WebRequest -UseBasicParsing "$url/PiArcBoot-Installer-Windows.zip" -OutFile "$dir\installer.zip"
        Expand-Archive "$dir\installer.zip" $dir
        Start-Process -Wait "$dir\Install PiArcBoot.exe"
    } finally {
        Remove-Item -Recurse -Force $dir
    }
}
