# Converts the home-screen backgrounds to the format the PS5 reads:
#   app/sce_sys/pic0-source.png -> pic0.dds   (shown when the title is selected)
#   app/sce_sys/pic1-source.png -> pic1.dds   (shown while the app starts)
# 3840x2160 BC7_UNORM DDS with a DX10 header and no mipmaps, made with
# Microsoft's texconv (DirectXTex, MIT), downloaded on first use.
#
#   powershell -File tools/make_dds.ps1
$ErrorActionPreference = 'Stop'
$sceSys = Join-Path $PSScriptRoot '..\app\sce_sys'
$texconv = Join-Path $PSScriptRoot 'texconv.exe'
if (-not (Test-Path $texconv)) {
    Invoke-WebRequest -UseBasicParsing -OutFile $texconv `
        -Uri 'https://github.com/microsoft/DirectXTex/releases/latest/download/texconv.exe'
}
$tmp = Join-Path ([IO.Path]::GetTempPath()) 'stremio-dds'
New-Item -ItemType Directory -Force $tmp | Out-Null
foreach ($pic in 'pic0', 'pic1') {
    $src = Join-Path $sceSys "$pic-source.png"
    & $texconv -nologo -y -w 3840 -h 2160 -m 1 -f BC7_UNORM -dx10 -o $tmp $src | Out-Null
    Copy-Item (Join-Path $tmp "$pic-source.dds") (Join-Path $sceSys "$pic.dds") -Force
    Write-Host "wrote app/sce_sys/$pic.dds"
}
