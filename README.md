# DualIC4VarjoApp_AdaptiveGazeGuidance

IC4Extで2台のIC4カメラをD3D12テクスチャとして取得し、`D3D12FrameSyncThread`で同期してVarjoXRのPlaneへ左右眼別に表示する実験用アプリケーションです。

通常レンダリングログに加えて、同じ実験フォルダへ次を記録します。

- `VarjoEyeTrackingService`
- `VarjoIMUService`
- `VarjoVSTService`
- レンダリングメタデータCSV

## 依存バージョン

- IC4Ext: `v1.0.1`
- VarjoXR: `v0.3.0`相当、commit `3d28a950b54edeafa3e7b595b78cc3b47cdc2ef5`
- VarjoToolkit: `v0.5.0`相当、commit `8512ba418dc0e63c67b6e0518ef84dc2b53fb39d`
- D3D12Helper: `v1.13.0`
- VarjoDualCameraApplicationsのキャリブレーション実装: commit `8470b8e34b0bdd50546cd2215c8969b0512c3eaa`
- OpenCV: vcpkg `x64-windows`

## スレッド構成

ライブキャリブレーション中は従来どおりmainスレッドで同期レンダリングします。キャリブレーション確定後の実験レンダリングは、`XRSpace`の専用レンダースレッドへ移します。

```text
IC4 camera 0 --\
                   D3D12FrameSyncThread
IC4 camera 1 --/          |
                            v
                     synchronized frame queue
                            |
                            v
XRSpace render thread
  +-- latest synchronized D3D12 resourcesを取得
  +-- latest HLSL constantsを反映
  +-- varjo_WaitSync 1回
  +-- Plane render / Varjo submit
  +-- FrameInfoSnapshotをdouble bufferへpublish
  +-- render metadataをlogger queueへenqueue

Main thread
  +-- FrameInfo double bufferを定期確認
  +-- frameNumberが変わった時だけEye Tracking / IMUへ提出
  +-- Eye Tracking application queueをdrain
  +-- 将来の時間変化HLSL constantsをpublish
  +-- stop / error管理
```

`XRSpace`、D3D12 backend、Plane、表示テクスチャリング、現在表示中の同期済みフレームは、実験開始後はレンダースレッドだけが操作します。

## FrameInfoの非同期共有

レンダースレッドは、成功した各`space.update()`の直後に同じ同期フレームの`VarjoFrameInfoSnapshot`をdouble bufferへ公開します。

mainは最新値を読み、`frameNumber`が前回提出値と異なる場合だけServiceへ渡します。

```cpp
std::int64_t lastSubmittedFrameNumber = -1;

if (const auto frameInfo = space.latestFrameInfoSnapshot();
    frameInfo && frameInfo->valid &&
    frameInfo->frameNumber != lastSubmittedFrameNumber) {

    serviceLogging.submitFrameInfo(*frameInfo);
    lastSubmittedFrameNumber = frameInfo->frameNumber;
}
```

これはlatest-only方式です。mainが長時間停止した場合は途中のFrameInfoを飛ばす可能性があるため、終了時に次を表示します。

```text
FrameInfo skipped by main latest-only polling
```

## HLSL定数の非同期共有

左右のHLSL user constantを同じrevisionへまとめてpublishします。

```cpp
VarjoXR::XRSpaceAsyncRenderState state;
state.revision = ++revision;
state.processingConstants.push_back(leftUpdate);
state.processingConstants.push_back(rightUpdate);
space.publishAsyncRenderState(std::move(state));
```

レンダースレッドはフレーム開始前に最新revisionだけを反映します。新しいrevisionがなければ前回値を使用します。

現在のステレオ補正定数は実験開始前に1回publishします。将来的に拍動、時間変化ブラー、視線追従強調などを行う場合、mainまたは制御スレッドで定数を生成し、同じ経路で非同期更新できます。

非同期定数変更をtexture変更なしでも反映できるよう、対象processingは`BeforeRenderEachFrame`へ設定します。

## 必要環境

- Windows 10/11 x64
- Visual Studio 2022
- CMake 3.21以上
- IC Imaging Control 4 SDK
- Varjo Native SDK / Varjo Runtime / Varjo HMD
- vcpkgのOpenCV `x64-windows`
- `ffmpeg.exe`がPATHから実行可能

## クリーンビルド（CMD）

VarjoXRの取得コミットが変わったため、以前のビルドディレクトリを削除してください。

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

通常実行時はD3D12デバッグレイヤーを無効にすることを推奨します。

```text
--d3d12-debug 0
```

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
  --metadata-csv rendered_frames.csv ^
  --d3d12-debug 0
```

## 実験出力

```text
<resolved project directory>/
  rendered_frames.csv
  eye_tracking.csv
  imu.csv
  varjo_vst_left.mp4
  varjo_vst_right.mp4
  varjo_vst_left_metadata.csv
  varjo_vst_right_metadata.csv
  varjo_service_summary.txt
```

同名プロジェクトが存在する場合は、`experiment01_1`、`experiment01_2`のように自動で別名を選びます。

## Varjoサービス

### Eye Tracking

- filter: `NONE`
- frequency: `MAXIMUM`
- Gaze、Eye Measurements、IPD、投影結果、対応FrameInfoを記録
- FrameInfo履歴は必要時刻範囲だけmutex内で検索・コピー
- 視線ごとの検索と投影計算はmutex外で実行

### IMU / Head Pose

- XRSpaceからmainへ公開された同じFrameInfoのCenter Poseを使用
- snapshot投入後の姿勢計算とCSV書き込みはIMUワーカースレッドで実行

### VST

- 左右MP4とmetadata CSVを出力
- Varjo DataStream経路を使用
- MP4エンコードにはPATH上の`ffmpeg.exe`を使用

## ライブキャリブレーション

```bat
--calib -
```

または、存在しないJSONパスを指定するとライブキャリブレーションを開始します。有効な推定後に`q`を押すと補正を確定します。

ライブキャリブレーション中はServiceログを開始しません。確定後にレンダースレッドを開始し、実験ログを記録します。

## Planeのキー操作

| キー | 動作 |
|---|---|
| ← / → | 左右へ0.01 m |
| ↑ / ↓ | 上下へ0.01 m |
| Shift + ↑ / ↓ | 奥 / 手前へ0.01 m |
| Shift + ← / → | 幅を0.01 m縮小 / 拡大。高さは縦横比を維持 |
| Esc / Ctrl+C | 終了 |

キーによるPlane変更もレンダースレッド上で処理します。
