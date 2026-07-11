# DualIC4VarjoApp_AdaptiveGazeGuidance

IC4Extで2台のIC4カメラをD3D12テクスチャとして取得し、`D3D12FrameSyncThread`で同期してVarjoXRの1枚のPlaneへ左右眼別に表示する実験用アプリケーションです。

通常のレンダリングログに加えて、VarjoToolkitの次のサービスを同じ実験フォルダへ記録します。

- `VarjoEyeTrackingService`
- `VarjoIMUService`
- `VarjoVSTService`

## 依存バージョン

- IC4Ext: `v1.0.1`
- VarjoXR: `v0.2.0`相当、commit `68fc5697c06ae842c0af3b9f647fb39ec9c3e019`
- VarjoToolkit: `v0.5.0`相当、commit `97ec3b3cc975455d8a664657e6596f62b2df0cff`
- D3D12Helper: `v1.13.0`
- VarjoDualCameraApplicationsのキャリブレーション実装: commit `8470b8e34b0bdd50546cd2215c8969b0512c3eaa`
- nlohmann/json: IC4Extが提供する`nlohmann_json::nlohmann_json`を共有
- OpenCV: vcpkgの`x64-windows`パッケージ。キャリブレーション解析ターゲットだけで使用

VarjoToolkitとVarjoXRは、外部FrameInfo入力化と`XRSpace::frameInfoSnapshot()`の実機テストが通った確定コミットへ固定しています。

## Varjoフレーム同期

`varjo_WaitSync`を呼ぶのはVarjoXRのD3D12レンダリングバックエンドだけです。

```text
VarjoXR D3D12 backend
    |
    +-- varjo_WaitSync() 1回
    |
    +-- VarjoFrameInfoSnapshotを保存
            |
            +-- Plane rendering
            +-- VarjoEyeTrackingService
            +-- VarjoIMUService
```

通常レンダリングでは各フレームで次の順に処理します。

```cpp
space.update();

const VarjoFrameInfoSnapshot frameInfo =
    space.frameInfoSnapshot();

serviceLogging.submitFrameInfo(frameInfo);
```

Eye TrackingとIMUは内部で`varjo_WaitSync`を呼びません。同じsnapshotを受け取り、GazeへのFrameInfo対応付けとHead Pose CSV出力に使用します。VSTは従来どおりVarjo DataStream経路です。

ライブキャリブレーション中はサービスがまだ起動していないため、Eye Tracking、IMU、VST、実験CSVへ校正中データは入りません。

## 処理構成

```text
IC4 camera 0 --\
                  D3D12FrameSyncThread -> synchronized D3D12 frame set
IC4 camera 1 --/                              |
                                                +-> fixed display texture ring
                                                |       -> VarjoXR Plane
                                                |          -> left/right HLSL remap
                                                |
                                                +-> calibration-only D3D12 copy/readback
                                                        -> OpenCV checkerboard analysis
                                                        -> latest JSON/snapshot

Shared Varjo session
  +-> VarjoXR D3D12 backend -> single WaitSync -> FrameInfoSnapshot
  |                                               +-> EyeTrackingService
  |                                               +-> IMUService
  +-> VarjoVSTService -> left/right MP4 and metadata CSV
```

視差補正はVarjoXR Planeの左右眼別`TextureProcessingDesc`へcompute HLSLを登録して行います。OpenCVはキャリブレーション用readback、チェッカーボード検出、行列推定だけで使用します。

## 必要環境

- Windows 10/11 x64
- Visual Studio 2022
- CMake 3.21以上
- IC Imaging Control 4 SDK
- Varjo Native SDK / Varjo Runtime / Varjo HMD
- vcpkgのOpenCV `x64-windows`
- `ffmpeg.exe`がPATHから実行可能

`ffmpeg.exe`はVarjoVSTServiceが左右VST映像をMP4へ保存するために使用します。

## ビルド（CMD）

VarjoToolkitとVarjoXRの取得コミットが変わったため、以前のビルドディレクトリは削除してください。

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

vcpkgはPATH上の`vcpkg.exe`または環境変数`VCPKG_ROOT`から自動検出します。明示的に`CMAKE_TOOLCHAIN_FILE`を指定した場合はそちらを優先します。

## 実験出力ディレクトリ

`--dir`と`--project`は必須です。

```text
--dir <親ディレクトリ>
--project <フォルダ名>
```

例えば次の指定では、`D:\experiments\suturing01`を作成します。

```bat
--dir "D:\experiments" --project suturing01
```

すでに同名が存在する場合は自動的に次の名前を試します。

```text
suturing01
suturing01_1
suturing01_2
...
```

`--project`にはパスではなく単一のフォルダ名を指定してください。

### 生成される主なファイル

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

`--metadata-csv FILENAME`を指定すると、`rendered_frames.csv`のファイル名だけを変更できます。指定値にディレクトリが含まれていても、ファイルは必ず解決済みプロジェクトフォルダ内に作成されます。

## Varjoサービスの記録期間

ライブキャリブレーションを行う場合、Eye Tracking、IMU、VST、レンダリングCSVはキャリブレーション確定後に開始します。そのため、チェッカーボードを提示している校正フェーズは実験ログへ混入しません。

3サービスのいずれかが開始できない場合、ログが部分的に欠けた状態で実験を続行せず、通常レンダリング開始前にエラー終了します。

各レンダリングフレームで取得したsnapshotをEye TrackingとIMUの両方が受理できなかった場合も、FrameInfoログ欠損としてエラー終了します。

### Eye Tracking

- 出力: `eye_tracking.csv`
- filter: `NONE`
- frequency: `MAXIMUM`
- Gaze、Eye Measurements、IPD、表示座標への投影結果、対応FrameInfoを記録
- VarjoXRが取得したFrameInfo履歴を最大512件保持してGaze時刻へ対応付け

### IMU / Head Pose

- 出力: `imu.csv`
- VarjoXRのsnapshotに含まれるCenter Poseを使用
- pose、position、Euler角、角速度、FrameInfo、Varjo時刻とUnix時刻を記録
- snapshot投入後の姿勢計算とCSV書き込みはIMUワーカースレッドで実行

### VST

- 出力: 左右MP4と左右metadata CSV
- VST映像、stream frame情報、intrinsics、extrinsics、Varjo時刻とUnix時刻を記録
- MP4エンコードにはPATH上の`ffmpeg.exe`を使用

終了時には各サービスを停止してファイルを閉じ、次の統計を`varjo_service_summary.txt`へ保存します。

- Gaze受信数、アプリケーションキューdrop数
- Eye Trackingへ投入したFrameInfo数と履歴drop数
- IMUの受信・処理・書き込み・drop数
- VST左右フレーム数、drop数、write failure数
- 各サービスのサンプルレート

## `--calib JSON_PATH|-`

`--calib`には必ず値を1つ指定します。

### 既存JSONを使用

```bat
--calib C:\calibration\stereo.json
```

指定ファイルが存在する場合は起動時に読み込み、直ちにPlaneの左右HLSLへ反映します。

### 新しいJSONを作成して保存

```bat
--calib C:\calibration\new_stereo.json
```

指定ファイルが存在しない場合は通常の実験処理へ入る前にライブ校正を開始します。有効な推定後に`q`を押すとJSONを保存し、その補正を維持したまま実験ログを開始します。

### 保存せずライブ校正

```bat
--calib -
```

`q`で確定した補正は表示へ引き継ぎますが、JSONは保存しません。

### 校正オプション

```text
--calib-board-cols N             既定: 12 inner corners
--calib-board-rows N             既定: 9 inner corners
--calib-profile NAME             既定: affine_vertical
                                  affine_vertical|affine_full|uncalibrated
--calib-max-observations N       既定: 30
--calib-min-observations N       既定: 8
--calib-min-corner-motion-px N   既定: 15
--calib-ransac-threshold-px N    既定: 1.5
--calib-sb 0|1                   低速なSB検出を使うか
```

## 現在の実機設定例

```bat
out\build\default\Release\DualIC4VarjoApp.exe ^
  --dir "D:\experiments" ^
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
  --camera-start-delay-ms 2000 ^
  --sync-timestamp host ^
  --sync-tolerance-ms 5.0 ^
  --calib "C:\Users\MiyafujiLab2\Downloads\stereo_calibration.json"
```

保存せず新規校正する場合は`--calib`行を次へ置き換えます。

```bat
  --calib -
```

## Planeのキー操作

操作は押下エッジで処理します。

| キー | 動作 |
|---|---|
| ← / → | 左右へ0.01 m |
| ↑ / ↓ | 上下へ0.01 m |
| Shift + ↑ / ↓ | 奥 / 手前へ0.01 m |
| Shift + ← / → | 幅を0.01 m縮小 / 拡大。高さは縦横比を維持 |
| Esc / Ctrl+C | 終了 |

## レンダリングメタデータCSV

`rendered_frames.csv`には以下を記録します。

- レンダー提出時刻と成功状態
- 新しい同期済みカメラフレームへ切り替えたか
- 使用中のキャリブレーションsource / profile / revision
- Plane位置、サイズ、移動・リサイズ操作
- 左右カメラframe numberとtimestamp
- 左右同期差
- FrameSyncThread、同期キュー、左右CameraCaptureThreadの累積統計

1レコードを一度文字列として作成し、CRLFを1回だけ付加するため、ロガー側から空行は生成しません。
