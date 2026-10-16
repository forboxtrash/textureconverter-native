# Конвертер текстур - Texture Converter, v3.4, автор Antoñŷ
# Сборка tc_native.so (WEBP и AVIF, чтение и запись) под Windows x64.
# Запускать из "x64 Native Tools Command Prompt for VS" (2022 или 2026).
# Пример:
#   powershell -ExecutionPolicy Bypass -File .\build_windows.ps1 `
#     -VcpkgRoot C:\vcpkg -RubyExamplesDir C:\src\ruby-c-extension-examples
# Результат: папка dist\native (tc_native.so и licenses), её содержимое кладётся
# в TextureConverter\native\ внутри плагина.
param(
  [Parameter(Mandatory=$true)][string]$VcpkgRoot,         # например C:\vcpkg
  [Parameter(Mandatory=$true)][string]$RubyExamplesDir,   # клон SketchUp/ruby-c-extension-examples
  [string]$RubyInclude = '',                              # по умолчанию ThirdParty\include\ruby\3.2\win32_x64
  [string]$RubyLib = '',                                  # по умолчанию ThirdParty\lib\win32\x64-ucrt-ruby320.lib
  [string]$Generator = '',                                # по умолчанию определяется по установленной Visual Studio
  [switch]$WithRaw,                                       # LibRaw, в 3.4 не нужен
  [switch]$WithHeif,                                      # libheif, в 3.4 не нужен (libheif в vcpkg.json вручную)
  [switch]$Clean                                          # удалить build и dist перед сборкой
)
$ErrorActionPreference = 'Stop'
$triplet = 'x64-windows-static-md'

if (-not $RubyInclude) { $RubyInclude = Join-Path $RubyExamplesDir 'ThirdParty\include\ruby\3.2\win32_x64' }
if (-not $RubyLib)     { $RubyLib     = Join-Path $RubyExamplesDir 'ThirdParty\lib\win32\x64-ucrt-ruby320.lib' }

$toolchain = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
foreach ($p in @(
    @($toolchain, 'vcpkg toolchain (проверьте -VcpkgRoot и что выполнен bootstrap-vcpkg.bat)'),
    @((Join-Path $RubyInclude 'ruby.h'), 'ruby.h (проверьте -RubyExamplesDir или -RubyInclude)'),
    @((Join-Path $RubyInclude 'ruby\config.h'), 'ruby\config.h в папке заголовков Ruby'),
    @($RubyLib, 'x64-ucrt-ruby320.lib (проверьте -RubyExamplesDir или -RubyLib)'))) {
  if (-not (Test-Path $p[0])) { throw "Не найден файл: $($p[0])  [$($p[1])]" }
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  throw 'cl.exe не найден: запускайте из "x64 Native Tools Command Prompt for VS" (Пуск, наберите x64 Native).'
}
if (-not $Generator) {
  $vsv = [string]$env:VisualStudioVersion
  if     ($vsv.StartsWith('18')) { $Generator = 'Visual Studio 18 2026' }
  elseif ($vsv.StartsWith('17')) { $Generator = 'Visual Studio 17 2022' }
  else { throw "Не удалось определить версию Visual Studio (VisualStudioVersion='$vsv'). Укажите -Generator 'Visual Studio 17 2022' или 'Visual Studio 18 2026'." }
}
Write-Host "Visual Studio: $Generator"

if ($Clean) {
  Remove-Item -Recurse -Force build, dist -ErrorAction SilentlyContinue
}

$raw  = if ($WithRaw)  { 'ON' } else { 'OFF' }
$heif = if ($WithHeif) { 'ON' } else { 'OFF' }
$cmakeArgs = @(
  '-S', '.', '-B', 'build', '-G', $Generator, '-A', 'x64',
  "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
  "-DVCPKG_TARGET_TRIPLET=$triplet",
  "-DSU_RUBY_INCLUDE=$RubyInclude", "-DSU_RUBY_LIB=$RubyLib",
  "-DTC_WITH_RAW=$raw", "-DTC_WITH_HEIF=$heif"
)
if ($WithRaw) { $cmakeArgs += '-DVCPKG_MANIFEST_FEATURES=raw' }

Write-Host '1/4 Настройка CMake и сборка кодеков через vcpkg (первый раз долго, aom собирается из исходников)...'
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "cmake (настройка) завершился с кодом $LASTEXITCODE" }

Write-Host '2/4 Сборка tc_native.so...'
& cmake --build build --config Release
if ($LASTEXITCODE -ne 0) { throw "cmake (сборка) завершился с кодом $LASTEXITCODE" }

$out = Join-Path (Get-Location) 'build\Release\tc_native.so'
if (-not (Test-Path $out)) { throw "Не собрался $out" }

Write-Host '3/4 Проверка файла (dumpbin)...'
$exports = & dumpbin /exports $out | Out-String
if ($exports -notmatch 'Init_tc_native') {
  throw 'В tc_native.so нет экспорта Init_tc_native: SketchUp не сможет его загрузить.'
}
Write-Host '  экспорт Init_tc_native: есть'
$deps = (& dumpbin /dependents $out | Out-String)
Write-Host '  зависимости (ожидаются x64-ucrt-ruby320.dll, системные DLL и vcruntime/ucrt; dll кодеков быть не должно):'
($deps -split "`r?`n" | Where-Object { $_ -match '\.dll' }) | ForEach-Object { Write-Host ('    ' + $_.Trim()) }
if ($deps -match 'webp|avif|dav1d|aom|yuv') {
  Write-Warning 'Среди зависимостей есть DLL кодеков: линковка не статическая, на другом ПК модуль не загрузится.'
}

Write-Host '4/4 Подготовка папки dist\native...'
$dist = Join-Path (Get-Location) 'dist\native'
New-Item -ItemType Directory -Force (Join-Path $dist 'licenses') | Out-Null
Copy-Item $out $dist -Force
$share = Join-Path (Get-Location) "build\vcpkg_installed\$triplet\share"
foreach ($port in 'libwebp', 'libavif', 'dav1d', 'aom', 'libyuv', 'libraw') {
  $src = Join-Path $share "$port\copyright"
  if (Test-Path $src) { Copy-Item $src (Join-Path $dist "licenses\$port.txt") -Force }
  elseif (($port -ne 'libraw') -or $WithRaw) { Write-Warning "Нет файла лицензии $src, возьмите текст из исходников порта." }
}

Write-Host ''
Write-Host "Готово: $dist"
Write-Host 'Дальше: selftest.rb в Ruby Console SketchUp (см. README, раздел 4), затем положить содержимое dist\native в TextureConverter\native\ плагина.'
