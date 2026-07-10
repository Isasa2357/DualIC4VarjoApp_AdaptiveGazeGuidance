# DualIC4VarjoApp_AdaptiveGazeGuidance

IC4Extで2台のカメラをD3D12テクスチャとして取得し、`D3D12FrameSyncThread`で同期した左右フレームを、VarjoXRの1枚のPlaneへ左右眼別テクスチャとして表示する実験用アプリケーションです。Varjoへ提出した各レンダーフレームと、その時点で表示していた左右カメラフレーム、Plane位置、同期状態をCSVへ記録します。

## 固定している依存バージョン

- IC4Ext: **v1.0.1**
- VarjoXR: **v0.1.0**
- D3D12Helper: **v1.12.1**

CMakeの`FetchContent`でタグを明示しています。VarjoXRとIC4Extは同じD3D12Helperターゲットを共有します。VarjoToolkitはVarjoXRが取得したものを、アプリケーションの`VarjoSession`にも使用します。

## 処理構成

```text
IC4 camera 0 -> D3D12CameraCaptureThread --\
                                              D3D12FrameSyncThread
IC4 camera 1 -> D3D12CameraCaptureThread --/          |
                                                     v
                                        D3D12SyncedFrameSet
                                                     |
                                                     v
                                   fixed D3D12 display texture ring
                                                     |
                         left texture -> VarjoXR Plane left eye
                        right texture -> VarjoXR Plane right eye
                                                     |
                                                     v
                                        rendered_frames_*.csv
```

同期済みカメラフレームはGPU上で固定数の表示用D3D12テクスチャへコピーします。CPU readbackは行いません。

## 必要環境

- Windows 10/11 x64
- Visual Studio 2022
- CMake 3.21以上
- IC Imaging Control 4 SDK
- Varjo Native SDK
- Varjo Base / Varjo Runtime
- Varjo HMD
- IC4で認識されるカメラ2台

## IC4 SDKの検出

CMakeは次の順でIC4 SDKを探し、`include/ic4/ic4.h`と`lib/x64/ic4core.lib`を確認します。

1. CMake変数`IC4_SDK_ROOT`
2. 環境変数`IC4_SDK_ROOT`
3. 環境変数`IC4PATH`
4. `%LOCALAPPDATA%\Programs\The Imaging Source Europe GmbH\IC Imaging Control 4`
5. `%ProgramFiles%\The Imaging Source Europe GmbH\IC Imaging Control 4`

見つからない場合は、Visual Studioのコンパイル中ではなくCMake構成時に停止します。

## ビルド（CMD）

リポジトリのルートへ移動済みの前提です。

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

## 現在の実機実行例

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
  --metadata-csv logs\experiment01_rendered_frames.csv
```

JSON使用時に`--width`、`--height`、`--fps`、`--format`を指定すると、その項目はJSONより後から上書きされます。offsetは常に左右個別の引数から指定できます。

## Planeのキー操作

操作は押下エッジで処理します。キーを押しっぱなしにしても連続移動せず、1回押すごとに0.01 m移動します。

| キー | 動作 |
|---|---|
| ← | 左へ0.01 m |
| → | 右へ0.01 m |
| ↑ | 上へ0.01 m |
| ↓ | 下へ0.01 m |
| Shift + ↑ | 奥へ0.01 m |
| Shift + ↓ | 手前へ0.01 m |
| Esc / Ctrl+C | 終了 |

移動時は現在の`x / y / z`をコンソールへ表示します。初期位置は実行引数`--plane-x-m`、`--plane-y-m`、`--plane-distance-m`で指定できます。

## offset引数

- `--left-offset-x N`: 左カメラの`OffsetX`
- `--left-offset-y N`: 左カメラの`OffsetY`
- `--right-offset-x N`: 右カメラの`OffsetX`
- `--right-offset-y N`: 右カメラの`OffsetY`

offsetはIC4 state JSON適用後、Width/Height設定後にカメラへ適用されます。

## 同期設定

既定では、独立したUSBカメラのdevice timestamp epochが一致しない可能性を考慮し、同一プロセスの`steady_clock`で記録された`hostReceivedTime`を比較します。

- `--sync-timestamp host`: host受信時刻を比較。既定値
- `--sync-timestamp device`: device timestampを比較
- `--sync-timestamp auto`: IC4Extの自動選択
- `--sync-tolerance-ms`: 左右を同一セットとみなす最大時刻差

レンダー直前には同期済み出力キューの最新セットを取得します。表示されずに破棄された古いセット数はCSVの`synced_queue_dropped_by_pop_latest`で確認できます。

## メタデータCSV

CSVはUTF-8 BOM付きです。Varjoへ提出したレンダーフレームごとに1行を記録します。書き込み側では、1レコードを一度文字列として組み立ててからCRLFを1回だけ付加するため、空行は生成しません。

記録する情報は次のとおりです。

- レンダー行番号、提出時刻、提出成功状態
- 新しい同期フレームへ切り替えたか
- Planeをそのフレームで移動したか
- Planeの配置方式と`x / y / z`
- sync group ID、同期セット生成時刻、左右時刻差
- 左右のframe number、device timestamp、host受信時刻、幅、高さ
- FrameSyncThreadの入力数、出力数、drop数、push failure数
- 同期済みキューのdrop数
- 左右CameraCaptureThreadのread数、timeout数、error数
- 表示用D3D12テクスチャリングのslot index

値が取得できないIC4 chunk metadata用の空列や、同じ情報を単位違いで重複させた列は作成していません。

## 主な引数

```text
--left-device-index N / --right-device-index N
--left-serial TEXT / --right-serial TEXT
--left-json PATH / --right-json PATH
--left-json-device-index N / --right-json-device-index N
--left-offset-x N / --left-offset-y N
--right-offset-x N / --right-offset-y N
--width N --height N --fps N --format FORMAT
--sync-tolerance-ms N
--sync-timestamp host|device|auto
--camera-start-delay-ms N
--placement head|world
--plane-width-m N --plane-height-m N
--plane-x-m N --plane-y-m N --plane-distance-m N
--metadata-csv PATH
--display-ring-size N
--max-runtime-seconds N
--d3d12-debug 0|1
```

全項目は`DualIC4VarjoApp.exe --help`で表示できます。
