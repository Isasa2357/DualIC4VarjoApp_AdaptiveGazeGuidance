# DualIC4VarjoApp_AdaptiveGazeGuidance

IC4Extで2台のIC4カメラをD3D12テクスチャとして取得し、`D3D12FrameSyncThread`で同期してVarjoXRのPlaneへ左右眼別に表示する実験用アプリケーションです。

現在のリビジョンではジッタ原因を切り分けるため、Eye Tracking、IMU、VSTなどのVarjoサービス処理はアプリケーションから削除しています。記録するのは`rendered_frames.csv`だけです。

## 依存バージョン

- IC4Ext: `v1.0.2`
- VarjoXR: `v0.3.0`相当、commit `3d28a950b54edeafa3e7b595b78cc3b47cdc2ef5`
- VarjoToolkit: `v0.5.0`相当、commit `8512ba418dc0e63c67b6e0518ef84dc2b53fb39d`
- D3D12Helper: `v1.13.0`
- VarjoDualCameraApplicationsのキャリブレーション実装: commit `8470b8e34b0bdd50546cd2215c8969b0512c3eaa`
- OpenCV: vcpkg `x64-windows`

## 通常レンダリング

通常レンダリングは`XRSpace`の専用レンダースレッドで実行します。

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
```

通常レンダリング開始後、`XRSpace`、D3D12 backend、`XRPlane`、`StereoDisplayTextureRing`、現在表示中の同期済みフレームはレンダースレッドだけが操作します。

## ライブキャリブレーションの動的キュー

IC4Ext v1.0.2の次のAPIを使用します。

```cpp
camera.addOutputQueue(cameraIndex, queue);
camera.removeOutputQueue(cameraIndex, queue);
camera.outputQueueCount();
```

ライブキャリブレーション開始時だけ、各`D3D12CameraCaptureThread`へ校正専用キューを追加します。

```text
CameraCaptureThread
  +-- calibration input queue  <- IC4Ext内部で独立D3D12コピー
  +-- display input queue      <- 元のD3D12フレーム
```

`copyPerOutputQueue=true`のとき、IC4Extは最後の出力へ元フレームをmoveし、それより前の出力へ独立コピーを作成します。そのため、キュー登録順を次に固定します。

```text
1. calibration input queue
2. display input queue
```

校正経路は独立した第2の`D3D12FrameSyncThread`を使用します。

```text
left/right camera
  +-- display copyなし経路 -> display FrameSyncThread -> Varjo表示
  +-- calibrationコピー経路 -> calibration FrameSyncThread
                                    |
                                    v
                         LiveStereoCalibration submission worker
                                    |
                         GPU ready待ち・OpenCV解析
```

以前は`LiveStereoCalibration::submitLatest()`が左右フレームを自前コピーし、そのready fenceをレンダリングと同じmainスレッドで待っていました。現在は次のように変更しています。

- 手動の`D3D12FrameCopier`を削除
- 校正用の独立コピーはIC4Extへ委譲
- ready fence待ちは`LiveStereoCalibration`内部のsubmission workerで実行
- レンダリングループは校正コピー完了を待たない
- 校正終了時に`removeOutputQueue()`で校正キューだけを解除
- 通常表示キューはそのまま維持

起動時と終了時には次の診断ログを出力します。

```text
[CALIB] Dynamic IC4Ext queues attached. leftOutputs=2 rightOutputs=2
[CALIB] Dynamic IC4Ext queues detached. leftRemoved=1 rightRemoved=1 leftOutputs=1 rightOutputs=1
```

## HLSL定数の非同期共有

左右のHLSL user constantは同じrevisionへまとめ、`XRSpaceAsyncRenderState`としてpublishします。

```cpp
VarjoXR::XRSpaceAsyncRenderState state;
state.revision = nextRevision;
state.processingConstants.push_back(leftUpdate);
state.processingConstants.push_back(rightUpdate);
space.publishAsyncRenderState(std::move(state));
```

## 必要環境

- Windows 10 / 11 x64
- Visual Studio 2022
- CMake 3.21以上
- IC Imaging Control 4 SDK
- Varjo Native SDK / Varjo Runtime / Varjo HMD
- vcpkgのOpenCV `x64-windows`

VST保存を削除したため、`ffmpeg.exe`は不要です。

## ビルド（CMD）

依存バージョンが変わっているため、以前のビルドディレクトリを削除してください。

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

## 実行例

既存のキャリブレーションJSONを使う場合:

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
  --d3d12-debug 0 ^
  --calib "C:\Users\MiyafujiLab2\Downloads\stereo_calibration.json" ^
  --metadata-csv rendered_frames.csv
```

保存せずライブキャリブレーションする場合は、`--calib`を次へ変更します。

```bat
  --calib - ^
```

## 出力

```text
<resolved project directory>/
  rendered_frames.csv
```

## Planeのキー操作

| キー | 動作 |
|---|---|
| ← / → | 左右へ0.01 m |
| ↑ / ↓ | 上下へ0.01 m |
| Shift + ↑ / ↓ | 前後へ0.01 m |
| Shift + ← / → | Plane幅を0.01 m変更 |
| Esc / Ctrl+C | 終了 |
