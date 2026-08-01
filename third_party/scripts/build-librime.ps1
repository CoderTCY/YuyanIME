# 构建时自动生成 librime.so（JNI 桥，供 APK 打包）
# 设计：librime.so 不进仓库，仅在 gradle 构建时由 NDK+CMake 编译生成。
# 幂等：目标 so 已存在且与构建产物一致时直接跳过（增量构建）。
# 用法：gradle 构建自动调用（yuyansdk/build.gradle 的 prepareLibrime 任务）；
#       也可手动执行：powershell -File third_party/scripts/build-librime.ps1
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$buildDir = Join-Path $root 'third_party\build-android'
$targetSo = Join-Path $root 'yuyansdk\libs\arm64-v8a\librime.so'

# 1. 目标已存在 → 跳过（不重复编译）
if (Test-Path $targetSo) {
    Write-Host "[build-librime] librime.so 已存在，跳过编译: $targetSo"
    exit 0
}

# 2. 定位 NDK：优先读 CMakeCache（已配置过），否则从 local.properties 的 sdk.dir 找最新 NDK
$ndkToolchain = $null
if (Test-Path (Join-Path $buildDir 'CMakeCache.txt')) {
    $m = Select-String -Path (Join-Path $buildDir 'CMakeCache.txt') -Pattern '^CMAKE_TOOLCHAIN_FILE:UNINITIALIZED=(.+)$'
    if ($m) { $ndkToolchain = $m.Matches[0].Groups[1].Value }
}
if (-not $ndkToolchain) {
    $props = Join-Path $root 'local.properties'
    if (Test-Path $props) {
        $m = Select-String -Path $props -Pattern '^sdk\.dir=(.+)$'
        if ($m) {
            $sdk = $m.Matches[0].Groups[1].Value -replace '\\', '\'
            $ndk = Get-ChildItem (Join-Path $sdk 'ndk') -Directory -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending | Select-Object -First 1
            if ($ndk) { $ndkToolchain = Join-Path $ndk.FullName 'build\cmake\android.toolchain.cmake' }
        }
    }
}
if (-not $ndkToolchain) {
    Write-Error "[build-librime] 找不到 NDK（需 local.properties 配置 sdk.dir，或已配置过 CMakeCache）"
}

# 3. configure（仅首次/缓存缺失时）
if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
    Write-Host "[build-librime] configure: cmake -S third_party -B third_party/build-android"
    & cmake -S (Join-Path $root 'third_party') -B $buildDir -G Ninja `
        "-DCMAKE_TOOLCHAIN_FILE=$ndkToolchain" `
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-23 -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { Write-Error "[build-librime] configure 失败 (exit $LASTEXITCODE)" }
}

# 4. 构建 target rime（产物 third_party/build-android/jni/librime.so）
Write-Host "[build-librime] cmake --build --target rime"
& cmake --build $buildDir --target rime --config Release
if ($LASTEXITCODE -ne 0) { Write-Error "[build-librime] 编译失败 (exit $LASTEXITCODE)" }

$builtSo = Join-Path $buildDir 'jni\librime.so'
if (-not (Test-Path $builtSo)) {
    Write-Error "[build-librime] 产物缺失: $builtSo"
}

# 5. 拷贝到打包目录（用 NDK llvm-strip 剥离符号表，4.5MB 版）
$destDir = Split-Path $targetSo
New-Item -ItemType Directory -Path $destDir -Force | Out-Null
Copy-Item $builtSo $targetSo -Force

$ndkRoot = Split-Path (Split-Path (Split-Path $ndkToolchain))  # .../build/cmake/xxx → ndk 根
$strip = Get-ChildItem (Join-Path $ndkRoot 'toolchains\llvm\prebuilt') -Directory -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($strip) {
    $stripExe = Join-Path $strip.FullName 'bin\llvm-strip.exe'
    if (Test-Path $stripExe) {
        & $stripExe --strip-unneeded $targetSo
        Write-Host "[build-librime] llvm-strip 剥离符号表完成"
    }
}

$size = (Get-Item $targetSo).Length
Write-Host "[build-librime] 已生成: $targetSo ($([math]::Round($size/1MB, 1)) MB)"
exit 0
