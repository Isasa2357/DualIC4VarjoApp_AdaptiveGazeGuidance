# DualIC4VarjoApp_AdaptiveGazeGuidance

IC4Extで2台のカメラをD3D12テクスチャとして取得し、`D3D12FrameSyncThread`で同期した左右フレームを、VarjoXRの1枚のPlaneへ左右眼別テクスチャとして表示する実験用アプリケーションです。Varjoへ提出した各レンダーフレームと、その時点で表示していた左右カメラフレームのメタデータをCSVへ記録します。

## 固定している依存バージョン

- IC4Ext: **v1.0.1**
- VarjoXR: **v0.1.0**（現在確認できるタグ付き最新版）
- D3D12Helper: **v1.12.1**

CMakeの`FetchContent`でタグを明示しています。VarjoXR v0.1.0が内部で指定する古いD3D12Helperコミットはトップレベルからv1.12.1へ上書きし、IC4Extと同じD3D12Helperターゲットを共有します。VarjoToolkitはVarjoXRが固定コミットを取得し、アプリケーションも`VarjoSession`のために同じターゲットを利用します。

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

VarjoXR v0.1.0の`wrapResource()`を新規カメラフレームごとに呼ぶと、SRVディスクリプタを継続的に消費します。本アプリでは長時間実験に対応するため、固定数の表示用D3D12テクスチャを作成し、それらだけを一度ラップします。同期済みカメラフレームはGPU上で表示用リングへコピーされます。CPU readbackは行いません。

## 必要環境

- Windows 10/11 x64
- Visual Studio 2022
- CMake 3.21以上
- IC Imaging Control 4 SDK
- Varjo Native SDK
- Varjo Base / Varjo Runtime
- Varjo HMD
- IC4で認識されるカメラ2台

IC4 SDKとVarjo Native SDKは自動取得されません。

### IC4 SDKの検出

CMakeは次の順でIC4 SDKを探し、`include/ic4/ic4.h`と`lib/x64/ic4core.lib`の存在を確認します。

1. CMake変数`IC4_SDK_ROOT`
2. 環境変数`IC4_SDK_ROOT`
3. 環境変数`IC4PATH`
4. `%LOCALAPPDATA%\Programs\The Imaging Source Europe GmbH\IC Imaging Control 4`
5. `%ProgramFiles%\The Imaging Source Europe GmbH\IC Imaging Control 4`

見つからない場合は、Visual Studioのコンパイル中ではなくCMake構成時に停止し、調べたパスを表示します。

## ビルド（CMD）

リポジトリのルートへ移動済みの前提です。IC4を別の場所へインストールした場合は、1行目を実際のSDKルートへ変更してください。

```bat
set "IC4_SDK_ROOT=%LOCALAPPDATA%\Programs\The Imaging Source Europe GmbH\IC Imaging Control 4"
set "VARJO_SDK_ROOT=C:\Program Files\Varjo\Varjo SDK"

rmdir /s /q out\build\default 2>nul

cmake -S . -B out\build\default -G "Visual Studio 17 2022" -A x64 ^
  "-DIC4_SDK_ROOT:PATH=%IC4_SDK_ROOT%" ^
  "-DVARJOXR_VARJO_SDK_ROOT:PATH=%VARJO_SDK_ROOT%" ^
  -DVARJOXR_FETCH_DEPENDENCIES:BOOL=ON ^
  -DVARJOXR_FETCH_DXC_RUNTIME:BOOL=ON

cmake --build out\build\default --config Release --parallel
```

`VarjoLib.dll`、`dxcompiler.dll`、`dxil.dll`、および検出できた`ic4core.dll`は実行ファイルの隣へコピーされます。

## 実行例

### IC Capture 4のJSONと左右別offsetを使用

IC Capture 4のexport JSONには`Width`、`Height`、`AcquisitionFrameRate`、`PixelFormat`などが入りますが、`OffsetX`と`OffsetY`が入っていないことがあります。本アプリではJSONを適用した後、次の引数でoffsetだけを明示的に上書きします。

```bat
out\build\default\Release\DualIC4VarjoApp.exe ^
  --left-device-index 0 ^
  --right-device-index 1 ^
  --left-json C:\config\gamma1.json ^
  --right-json C:\config\gamma1.json ^
  --left-json-device-index 0 ^
  --right-json-device-index 0 ^
  --left-offset-x 0 ^
  --left-offset-y 0 ^
  --right-offset-x 128 ^
  --right-offset-y 0 ^
  --camera-start-delay-ms 2000 ^
  --sync-timestamp host ^
  --sync-tolerance-ms 5.0 ^
  --placement head ^
  --plane-width-m 1.0 ^
  --plane-distance-m 1.0 ^
  --metadata-csv logs\experiment01_rendered_frames.csv
```

左右で同じoffsetを使う場合も、左右両方へ同じ値を指定してください。

```bat
  --left-offset-x 128 --left-offset-y 64 ^
  --right-offset-x 128 --right-offset-y 64
```

JSON使用時に`--width`、`--height`、`--fps`を指定すると、その項目もJSONの値より後から上書きされます。`--format`も同様です。指定しなければJSON内の値を使用します。

### JSONを使用せずカメラ番号と形式を指定

```bat
out\build\default\Release\DualIC4VarjoApp.exe ^
  --left-device-index 0 ^
  --right-device-index 1 ^
  --width 1920 ^
  --height 1080 ^
  --fps 120 ^
  --format BGR8 ^
  --left-offset-x 0 ^
  --left-offset-y 0 ^
  --right-offset-x 0 ^
  --right-offset-y 0 ^
  --sync-tolerance-ms 5.0 ^
  --metadata-csv logs\experiment01_rendered_frames.csv
```

終了はEscまたはCtrl+Cです。

## offset引数

- `--left-offset-x N`: 左カメラの`OffsetX`
- `--left-offset-y N`: 左カメラの`OffsetY`
- `--right-offset-x N`: 右カメラの`OffsetX`
- `--right-offset-y N`: 右カメラの`OffsetY`

offsetは`CameraStreamRequest::offsetX/offsetY`へ設定され、IC4 state JSON適用後、Width/Height設定後にカメラへ適用されます。許容範囲や増分はカメラ機種と現在のROIサイズによって異なります。

## 同期設定

既定では、独立したUSBカメラのdevice timestamp epochが一致しない可能性を考慮し、同一プロセスの`steady_clock`で記録された`hostReceivedTime`を比較します。

- `--sync-timestamp host`: host受信時刻を比較。既定値
- `--sync-timestamp device`: device timestampを比較。カメラクロックが同期済みの場合のみ使用
- `--sync-timestamp auto`: IC4Extの自動選択
- `--sync-tolerance-ms`: 左右を同一セットとみなす最大時刻差

同期後の出力キューからはレンダー直前に最新セットを取り出します。表示遅延を抑えるため、古い同期済みセットが表示されず破棄される場合があります。この数はCSVの`synced_queue_dropped_by_pop_latest`で確認できます。

## メタデータCSV

CSVはUTF-8 BOM付きです。Varjoへ提出したレンダーフレームごとに1行を書きます。

主な列:

- レンダー提出時刻: Unix microseconds / local ISO 8601 / steady microseconds
- `new_frame_from_queue`: このレンダーフレームで新しい同期セットへ切り替えたか
- `submit_ok`: VarjoXRの`space.update()`が成功したか
- sync group ID、同期セット生成時刻、左右時刻差
- 左右のframe number、device timestamp、host received time
- 画像サイズとDXGI format
- IC4 chunk metadataと各`has_*`フラグ
- FrameSyncThread、同期済みキュー、左右CameraCaptureThreadの累積統計
- 表示用D3D12テクスチャリングのslot index

書き込みは専用スレッドで行い、各行をflushします。実験中の異常終了時にも直前までのログを残すことを優先しています。

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
