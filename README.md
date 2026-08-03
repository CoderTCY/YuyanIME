# 语燕输入法（个人 fork）

本项目为上游[语燕输入法](https://github.com/gurecn/YuyanIme)的个人 fork，基于 Rime 引擎的 Android 输入法。相比上游做出了以下更改：

## 主要更改

1. **手写输入模型更新**。
2. **引擎替换为原版 librime**：原有的闭源 `libyuyanime.so` 换成原版 librime（submodule，版本锁定），集成[雾凇拼音](https://github.com/iDvel/rime-ice)全量词库（810w）。
3. **候选词预测系统**：集成 [librime-predict](https://github.com/rime/librime-predict)，预测库为**简繁双向**（官方统计源繁→简 + 自动 s→t 繁体集 + rime-predict-zh 补充，约 96 万条目）。
4. **动态学习**：基于 commit_history 的 bigram 统计，用户输入自动学习联想词，落盘 `user_predict.txt`。

## 构建

```bash
# 拉取 submodule（librime、librime-predict、darts-clone、snappy）
git submodule update --init --recursive

# 编译 JNI（librime.so 不入库，构建时自动生成）
powershell -File third_party/scripts/build-librime.ps1

# 构建 release APK（需要 JDK 21 + Android SDK + NDK）
gradlew.bat :app:assembleOfflineRelease
```
