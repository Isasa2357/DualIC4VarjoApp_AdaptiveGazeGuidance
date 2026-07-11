# DualIC4VarjoApp_AdaptiveGazeGuidance

IC4Extで2台のIC4カメラをD3D12テクスチャとして取得し、`D3D12FrameSyncThread`で同期してVarjoXRのPlaneへ左右眼別に表示する実験用アプリケーションです。

現在のリビジョンでは、ジッタ原因を切り分けるため、次のVarjoサービス処理をアプリケーションから削除しています。

- Eye Tracking
- IMU / Head Pose logging
- VST動画・メタデータ保存

アプリケーションが記録するのはレンダリングメタデータCSVだけです。

## 依存バージョン

- IC4Ext: `v1.0.1`
- VarjoXR: `v0.3.0`相当、commit `3d28a950b54edeafa3e7b595b78cc3b47cdc2ef5`
- VarjoToolkit: `v0.5.0`相当、commit `8512ba418dc0e63c67b6e0518ef84dc2b53fb39d`
- D3D12Helper: `v1.13.0`
- VarjoDualCameraApplicationsのキャリブレーション実装: commit `8470b8e34b0bdd50546cd2215c8969b0512c3eaa`
- OpenCV: vcpkg `x64-windows`

VarjoToolkitはVarjo SessionとVarjoXRの内部依存として使用しますが、アプリケーションからサービスクラスは生成しません。

## スレッド構成

ライブキャリブレーション中はmainスレッドで同期レンダリングします。キャリブレーション確定後の通常レンダリングは、`XRSpace`の専用レンダースレッドで実行します。

```text
IC4 camera 0 --\
                    D3D12FrameSyncThread
IC4 camera 1 --/          |
                             v
                      synchronized frame queue
                             |
                             v
XRSpace render thread
  +-- 最新の同期済みD3D12 Resourceを取得
  +-- 最新のHLSL constantsを反映
  +-- varjo_WaitSyncを1回実行
  +-- Plane render / Varjo submit
  +-- rendered_frames.csv用データをlogger queueへ投入

Main thread
  +-- Esc / 最大実行時間を監視
  +-- レンダースレッド例外を監視
  +-- 将来のHLSL constants更新をpublish可能
  +-- stop処理
```

通常レンダリング開始後、次はレンダースレッドだけが操作します。

- `XRSpace`
- D3D12 backend
- `XRPlane`
- `StereoDisplayTextureRing`
- 現在表示中の同期済みフレーム

mainスレッドはFrameInfoを読み出したり、Eye Tracking・IMUへ提出したりしません。

## HLSL定数の非同期共有

左右のHLSL user constantは同じrevisionへまとめ、`XRSpaceAsyncRenderState`としてpublishします。

```cpp
VarjoXR::XRSpaceAsyncRenderState state;
state.revision = nextRevision;
state.processingConstants.push_back(leftUpdate);
state.processingConstants.push_back(rightUpdate);
space.publishAsyncRenderState(std::move(state));
```

レンダースレッドは各フレーム開始前に最新revisionを確認し、新しいstateだけをPlaneへ反映します。途中のstateはlatest-only方式で上書きできます。

現在はキャリブレーション確定後に初期定数を1回publishします。将来、時間的に変化する表示を追加する場合はmainまたは制御用スレッドから新しいrevisionをpublishします。

## 必要環境

- Windows 10 / 11 x64
- Visual Studio 2022
- CMake 3.21以上
- IC Imaging Control 4 SDK
- Varjo Native SDK / Varjo Runtime / Varjo HMD
- vcpkgのOpenCV `x64-windows`

VST保存を削除したため、`ffmpeg.exe`は不要です。

## ビルド（CMD）

依存コミットやソース構成が変わっているため、以前のビルドディレクトリを削除してください。

```bat
set "IC4_SDK_ROOT=%LOCALAPPDATA%\Programs\The Imaging Source Europe GmbH\IC Imaging Control 4"
set "VARJO_SDK_ROOT=C:\personal\iwatake\Varjo_Experimental_SDK_for_Custom_Engines_4.11.0 (1)\varjo-sdk-experimental"
set "PATH=%IC4_SDK_ROOT%\bin\x64;%VARJO_SDK_ROOT%\bin;%PATH%"

rmdir /s /q out\build\default 2>nul

cmake -S . -B out\build\default -G "Visual Studio 17 2022" -A x64 ^
  "-DIC4_SDK_ROOT:PATH=%IC4_SDK_ROOT%" ^
  "-DVARJOXR_VARJO_SDK_ROOT:PATH=%VARJO_SDK_ROOT%" ^
  -DVARJOXR_FETCH_DEPENDENCIES:BOOL=ON ^
  -DVARJOXR_FETCH_DXC_RUNTIME:BOOL=ON

cmake --build out\build\default --config Release --parallel
```

## 実験出力ディレクトリ

`--dir`と`--project`は必須です。

```text
--dir <親ディレクトリ>
--project <フォルダ名>
```

生成されるファイルは次です。

```text
<resolved project directory>/
  rendered_frames.csv
```

次のファイルは現在生成しません。

```text
eye_tracking.csv
imu.csv
varjo_vst_left.mp4
varjo_vst_right.mp4
varjo_vst_left_metadata.csv
varjo_vst_right_metadata.csv
varjo_service_summary.txt
```

`--metadata-csv FILENAME`を指定すると、`rendered_frames.csv`のファイル名を変更できます。

## 実行例

```bat
out\build\default\Release\DualIC4VarjoApp.exe ^
  --dir "logs" ^
  --project experiment01 ^
  --left-device-index 1 ^
  --right-device-index 0 ^
  --left-json "C:\Users\MiyafujiLab2\Downloads\gamma1.json" ^
  --right-json "C:\Users\MiyafujiLab2\Downloads\gamma1.json" ^
  --left-json-device-index 0 ^
  --right-json-device-index 0 ^
  --left-offset-x 236 ^
  --left-offset-y 0 ^
  --right-offset-x 236 ^
  --right-offset-y 0 ^
  --fps 160 ^
  --camera-start-delay-ms 2000 ^
  --sync-timestamp host ^
  --sync-tolerance-ms 5.0 ^
  --calib "C:\Users\MiyafujiLab2\Downloads\stereo_calibration.json" ^
  --metadata-csv rendered_frames.csv
```

保存せずライブキャリブレーションする場合は、`--calib`を次へ変更します。

```bat
  --calib - ^
```

## Planeのキー操作

操作は押下エッジで処理します。

| キー | 動作 |
|---|---|
| ← / → | 左右へ0.01 m |
| ↑ / ↓ | 上下へ0.01 m |
| Shift + ↑ / ↓ | 前後へ0.01 m |
| Shift + ← / → | Plane幅を0.01 m変更 |
| Esc / Ctrl+C | 終了 |

## 現在の目的

このサービスなし構成は、レンダリングジッタがEye Tracking、IMU、VST、または共通Varjo Sessionへの並行アクセスによるものかを切り分けるための基準実装です。
