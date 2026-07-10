# DualIC4VarjoApp_AdaptiveGazeGuidance

IC4Extで2台のIC4カメラをD3D12テクスチャとして取得し、`D3D12FrameSyncThread`で同期してVarjoXRの1枚のPlaneへ左右眼別に表示する実験用アプリケーションです。

## 依存バージョン

- IC4Ext: `v1.0.1`
- VarjoXR: `v0.1.0`
- D3D12Helper: `v1.12.1`
- VarjoDualCameraApplicationsのキャリブレーション実装: commit `8470b8e34b0bdd50546cd2215c8969b0512c3eaa`
- nlohmann/json: IC4Extが提供する`nlohmann_json::nlohmann_json`を共有
- OpenCV: vcpkgの`x64-windows`パッケージ。キャリブレーション解析ターゲットだけで使用

OpenCV型は通常表示コード、CSV、JSONモデル、Plane表示APIには露出しません。OpenCVを使うのはD3D12 readback後のチェッカーボード検出と視差推定だけです。

## 処理構成

```text
IC4 camera 0 --\
                  D3D12FrameSyncThread -> synchronized D3D12 frame set
IC4 camera 1 --/                              |
                                                +-> fixed display texture ring
                                                |       -> VarjoXR Plane
                                                |          -> left/right HLSL remap
                                                |
                                                +-> calibration-only D3D12 readback
                                                        -> OpenCV checkerboard analysis
                                                        -> latest JSON/snapshot
```

視差補正は追加の表示用コピー処理ではなく、VarjoXR Planeの左右眼別`TextureProcessingDesc`へcompute HLSLを登録して行います。JSONの`leftInverse`と`rightInverse`を定数バッファとして渡し、補正後のテクスチャをPlaneへ表示します。

## 必要環境

- Windows 10/11 x64
- Visual Studio 2022
- CMake 3.21以上
- IC Imaging Control 4 SDK
- Varjo Native SDK / Varjo Runtime / Varjo HMD
- vcpkgのOpenCV x64-windows

vcpkgはPATH上の`vcpkg.exe`または環境変数`VCPKG_ROOT`から自動検出します。明示的に`CMAKE_TOOLCHAIN_FILE`を指定した場合は、そちらを優先します。

## ビルド（CMD）

キャリブレーション依存とtoolchainが追加されたため、以前のビルドディレクトリは削除してください。

```bat
set "IC4_SDK_ROOT=%LOCALAPPDATA%\Programs\The Imaging Source Europe GmbH\IC Imaging Control 4"
set "VARJO_SDK_ROOT=C:\personal\iwatake\Varjo_Experimental_SDK_for_Custom_Engines_4.11.0 (1)\varjo-sdk-experimental"
set "PATH=%IC4_SDK_ROOT%\bin\x64;%PATH%"

rmdir /s /q out\build\default 2>nul

cmake -S . -B out\build\default -G "Visual Studio 17 2022" -A x64 ^
  "-DIC4_SDK_ROOT:PATH=%IC4_SDK_ROOT%" ^
  "-DVARJOXR_VARJO_SDK_ROOT:PATH=%VARJO_SDK_ROOT%" ^
  -DVARJOXR_FETCH_DEPENDENCIES:BOOL=ON ^
  -DVARJOXR_FETCH_DXC_RUNTIME:BOOL=ON

cmake --build out\build\default --config Release --parallel
```

vcpkgを自動検出できない場合:

```bat
cmake -S . -B out\build\default -G "Visual Studio 17 2022" -A x64 ^
  "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=C:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake" ^
  "-DIC4_SDK_ROOT:PATH=%IC4_SDK_ROOT%" ^
  "-DVARJOXR_VARJO_SDK_ROOT:PATH=%VARJO_SDK_ROOT%"
```

## `--calib JSON_PATH|-`

`--calib`には必ず値を1つ指定します。Boost.Program_optionsは使用せず、`-`を特別値にすることで既存の軽量な引数パーサで曖昧なく処理します。

### 1. 既存JSONを使用

```bat
DualIC4VarjoApp.exe --calib C:\calibration\stereo.json ...
```

指定ファイルが存在する場合は起動時にnlohmann/jsonで読み込み、カメラ出力サイズとの整合性を確認して、直ちにPlaneの左右HLSLへ反映します。ライブ校正フェーズには入りません。

### 2. 新しいJSONを作成して保存

```bat
DualIC4VarjoApp.exe --calib C:\calibration\new_stereo.json ...
```

指定ファイルが存在しない場合は、通常の実験処理へ入る前にライブ校正フェーズを開始します。

1. チェッカーボードを左右カメラへ見せる
2. 位置と角度を変えて複数観測を登録する
3. 新しいrevisionが生成されるたびに補正をPlane HLSLへライブ反映する
4. 有効な推定後に`q`を押す
5. JSONを指定パスへatomic saveする
6. 同じ補正を維持したまま通常の実験表示へ移る

必要観測数に達していない状態で`q`を押した場合は終了せず、現在の採用観測数を表示します。

### 3. 保存せずライブ校正

```bat
DualIC4VarjoApp.exe --calib - ...
```

`-`を指定するとライブ校正を行います。`q`で確定した補正は通常表示へ引き継ぎますが、ローカルJSONは作成しません。

`--calib`単独は使用できません。値がなければ引数エラーになります。

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

既存キャリブレーションを使用する場合:

```bat
out\build\default\Release\DualIC4VarjoApp.exe ^
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
  --calib "C:\Users\MiyafujiLab2\Downloads\stereo_calibration.json" ^
  --metadata-csv logs\experiment01_rendered_frames.csv
```

保存せず新規校正する場合は、上の`--calib`行を次へ置き換えます。

```bat
  --calib - ^
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

## メタデータCSV

ライブ校正フェーズのフレームは実験CSVへ記録しません。校正を確定して通常表示へ移った時点から記録を開始します。

追加した校正列:

- `calibration_source`: `none` / `json` / `live`
- `calibration_profile`
- `calibration_revision`

そのほか、Plane位置・サイズ、左右フレーム番号と時刻、同期差、FrameSyncThreadとカメラスレッドのdrop/error統計を記録します。1レコードを文字列化してCRLFを1回だけ追加するため、ロガー側から空行は生成しません。
