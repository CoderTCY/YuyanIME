# rime-ice 预构建包适配脚本
#
# 词库来源：rime-ice nightly release 的 full_compiled.zip（CI 用 rime CLI 预编译的
# 完整包：build/ 下展开 schema + prism/table bin，平台无关）。
#
# 适配（对齐项目现有 9 方案的 schema id，app 层零改动）：
#   rime_ice          -> pinyin（全拼）
#   double_pinyin     -> double_pinyin_natural（自然码双拼）
#   melt_eng          -> english（英文）
#   t9                -> t9_pinyin（九键，删 t9_processor——仓输入法扩展，官方 librime 无）
#   abc/flypy/mspy/sogou/ziguang 保持原名（裁剪 lua）
#   stroke / handwriting：项目自有（stroke 用旧 bin + 标准展开 schema；handwriting 占位）
# 通用裁剪：删 lua_processor/lua_translator/lua_filter（官方 librime 无 lua 插件）、
#   radical 拆字反查（依赖额外词典）、recognizer 的 lua patterns；
#   menu/page_size 5 -> 100（setRimePageSize no-op 的配套方案）。
#
# 用法：powershell -File third_party/scripts/prepare-rime-ice.ps1

param(
  [string]$OutDir = "",        # 输出目录（默认 yuyansdk/src/main/assets/rime）
  [switch]$SkipDownload        # 跳过下载（用缓存）
)

$ErrorActionPreference = "Stop"
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not $OutDir) { $OutDir = Join-Path $ProjectRoot "yuyansdk/src/main/assets/rime" }
$CacheDir = Join-Path $env:LOCALAPPDATA "yuyanime-rime-ice"
$ZipPath = Join-Path $CacheDir "full_compiled.zip"
$Staging = Join-Path $CacheDir "staging"
$UrlMirror = "https://mirror.nju.edu.cn/github-release/iDvel/rime-ice/LatestRelease/full_compiled.zip"
$UrlGitHub = "https://github.com/iDvel/rime-ice/releases/download/nightly/full_compiled.zip"

Write-Host "== 1/5 下载预构建包（缓存: $CacheDir）"
if (-not (Test-Path $ZipPath) -and -not $SkipDownload) {
  New-Item -ItemType Directory -Force -Path $CacheDir | Out-Null
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
  $wc = New-Object System.Net.WebClient
  $wc.Headers.Add("User-Agent", "yuyanime-build")
  try {
    Write-Host "  镜像源: $UrlMirror"
    $wc.DownloadFile($UrlMirror, $ZipPath)
  } catch {
    Write-Host "  镜像失败，回退 GitHub（代理）: $UrlGitHub"
    if ($env:https_proxy) { $wc.Proxy = New-Object System.Net.WebProxy($env:https_proxy) }
    $wc.DownloadFile($UrlGitHub, $ZipPath)
  }
  Write-Host "  下载完成: $([math]::Round((Get-Item $ZipPath).Length / 1MB, 1)) MB"
} elseif (Test-Path $ZipPath) {
  Write-Host "  使用缓存: $ZipPath"
} else {
  throw "缓存不存在且 -SkipDownload 已指定"
}

Write-Host "== 2/5 解压到 staging"
if (Test-Path $Staging) { Remove-Item -Recurse -Force $Staging }
New-Item -ItemType Directory -Force -Path $Staging | Out-Null
tar -xf $ZipPath -C $Staging
$Build = Join-Path $Staging "build"

# 改名映射：旧 schema_id -> 新 schema_id（文件名同步改）
$rename = @{
  "rime_ice"          = "pinyin"
  "double_pinyin"     = "double_pinyin_natural"
  "melt_eng"          = "english"
  "t9"                = "t9_pinyin"
}

function Strip-Schema {
  # 裁剪展开版 schema：删 lua/t9_processor/radical 引用、改 page_size、改名
  param([string]$File, [string]$NewId)
  $out = New-Object System.Collections.Generic.List[string]
  foreach ($line in (Get-Content $File)) {
    $t = $line.Trim()
    if ($t -match '^"?- "?lua_(processor|translator|filter)@') { continue }
    if ($t -eq "- t9_processor") { continue }
    if ($t -match '^- "?(affix_segmentor@radical_lookup|reverse_lookup_filter@)') { continue }
    if ($t -match '^(calculator|unicode|number|gregorian_to_lunar|radical_lookup): ') { continue }
    if ($t -match '^page_size: 5$') { $out.Add("  page_size: 100"); continue }
    if ($NewId -and $t -match '^schema_id: ') { $out.Add("  schema_id: $NewId"); continue }
    if ($NewId -and $t -match '^prism: ') { $out.Add("  prism: $NewId"); continue }
    $out.Add($line)
  }
  Set-Content -Path $File -Value $out -Encoding UTF8
}

Write-Host "== 3/5 裁剪 + 改名 schema（build/ 展开版）"
$keep = @("double_pinyin_abc", "double_pinyin_flypy", "double_pinyin_mspy",
          "double_pinyin_sogou", "double_pinyin_ziguang")
$schemas = Get-ChildItem $Build -Filter "*.schema.yaml" | ForEach-Object { $_.BaseName -replace "\.schema$", "" }
foreach ($s in $schemas) {
  $src = Join-Path $Build "$s.schema.yaml"
  if ($rename.ContainsKey($s)) {
    $newId = $rename[$s]
    Strip-Schema $src $newId
    $dst = Join-Path $Build "$newId.schema.yaml"
    Move-Item -Force $src $dst
    # prism bin 复制改名（table/reverse 保持原名：dictionary 引用不变）
    foreach ($ext in @("prism.bin")) {
      $b = Join-Path $Build "$s.$ext"
      if (Test-Path $b) { Copy-Item $b (Join-Path $Build "$newId.$ext") -Force }
    }
    Write-Host "  $s -> $newId"
  } elseif ($keep -contains $s) {
    Strip-Schema $src $null
    Write-Host "  $s（保持，已裁剪）"
  } else {
    Remove-Item $src -Force
    Write-Host "  $s（跳过）"
  }
}

# 删掉不用的 bin（radical_pinyin / jiajia / 其他）
Get-ChildItem $Build -Filter "radical_pinyin.*" -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem $Build -Filter "double_pinyin_jiajia.*" -ErrorAction SilentlyContinue | Remove-Item -Force

Write-Host "== 4/5 stroke / handwriting 展开 schema + default.yaml"
@"
schema:
  schema_id: stroke
  name: 五笔画
  version: "1.0"
  engine:
    processors:
      - speller
      - selector
      - navigator
      - express_editor
    segmentors:
      - abc_segmentor
      - fallback_segmentor
    translators:
      - table_translator
  speller:
    alphabet: "hspnz"
    delimiter: " "
    algebra: []
  translator:
    dictionary: stroke
    preedit_format:
      - xform/h/一/
      - xform/s/丨/
      - xform/p/丿/
      - xform/n/丶/
      - xform/z/乙/
  menu:
    page_size: 100
"@ | Set-Content -Path (Join-Path $Build "stroke.schema.yaml") -Encoding UTF8
Write-Host "  stroke.schema.yaml（展开版，配旧 stroke bin）"

@"
schema:
  schema_id: handwriting
  name: 手写
  version: "1.0"
  engine:
    processors:
      - speller
    segmentors:
      - fallback_segmentor
    translators:
      - table_translator
  speller:
    alphabet: abcdefghijklmnopqrstuvwxyz
    delimiter: " "
  translator:
    dictionary: ""
  menu:
    page_size: 100
"@ | Set-Content -Path (Join-Path $Build "handwriting.schema.yaml") -Encoding UTF8
Write-Host "  handwriting.schema.yaml（占位）"

# default.yaml：page_size 5 -> 100（build/ 展开版 + 根目录各一份，保留原缩进）
foreach ($f in @((Join-Path $Build "default.yaml"), (Join-Path $Staging "default.yaml"))) {
  if (Test-Path $f) {
    (Get-Content $f) | ForEach-Object {
      if ($_ -match '^( *page_size:)\s*5(\s*#.*)?$') { -join ($Matches[1], " 100") } else { $_ }
    } | Set-Content -Path $f -Encoding UTF8
  }
}
Write-Host "  default.yaml page_size -> 100"

Write-Host "== 5/5 安装到 $OutDir"
$OutBuild = Join-Path $OutDir "build"
# 备份旧 stroke bin（复用，不重新编译）——先复制到 staging 临时区，删除 build/ 后仍可用
$StrokeBackup = Join-Path $Staging "stroke-backup"
New-Item -ItemType Directory -Force -Path $StrokeBackup | Out-Null
if (Test-Path $OutBuild) {
  foreach ($n in @("stroke.prism.bin", "stroke.table.bin")) {
    $p = Join-Path $OutBuild $n
    if (Test-Path $p) { Copy-Item $p $StrokeBackup -Force }
  }
  Remove-Item -Recurse -Force $OutBuild
}
New-Item -ItemType Directory -Force -Path $OutBuild | Out-Null
# 拷贝 build/
Copy-Item (Join-Path $Build "*") $OutBuild -Recurse -Force
# 恢复旧 stroke bin
Copy-Item (Join-Path $StrokeBackup "*") $OutBuild -Force -ErrorAction SilentlyContinue
# 根目录文件（default.yaml / custom_phrase.txt / symbols_v.yaml）
foreach ($n in @("default.yaml", "custom_phrase.txt", "symbols_v.yaml")) {
  $p = Join-Path $Staging $n
  if (Test-Path $p) { Copy-Item $p (Join-Path $OutDir $n) -Force }
}
# opencc/：CI 的 emoji 数据合入（保留旧 s2t 数据）
if (Test-Path (Join-Path $Staging "opencc")) {
  New-Item -ItemType Directory -Force -Path (Join-Path $OutDir "opencc") | Out-Null
  Copy-Item (Join-Path $Staging "opencc/*") (Join-Path $OutDir "opencc") -Force
}

Write-Host ""
Write-Host "=== 最终 build/ 内容 ==="
Get-ChildItem $OutBuild -File | Sort-Object Name | ForEach-Object {
  "{0,-40} {1,8:N0} KB" -f $_.Name, ($_.Length/1KB)
}
