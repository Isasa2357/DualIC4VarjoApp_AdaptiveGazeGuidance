# DualIC4VarjoApp_AdaptiveGazeGuidance

IC4Extで2台のIC4カメラをD3D12テクスチャとして取得し、同期してVarjoXRのPlaneへ左右眼別に表示する実験用アプリケーションです。

このブランチではIC4Ext v2のReadOnly共有フレーム方式へ移行しています。カメラごとに出力先の数だけGPUテクスチャを複製するのではなく、左右カメラは1本の中央ingressへフレームを発行し、1本の中央`FrameSyncThread`が同期済み`ReadOnlyFrameSet`を複数consumerへfan-outします。

カメラの指定は**シリアルIDのみ**です。カメラインデックスやunique nameによるカメラ選択は使用できません。

## 依存バージョン

- IC4Ext: v2 shared ReadOnly pipeline、commit `abb4488eeb63a06464a5859e1d488092f7e71789`
- VarjoXR: commit `3d28a950b54edeafa3e7b595b78cc3b47cdc2ef5`
- VarjoToolkit: commit `8512ba418dc0e63c67b6e0518ef84dc2b53fb39d`
- D3D12Helper: `v1.13.0`
- OpenCV: vcpkg `x64-windows`

## IC4Ext v2の共有フレーム構成

```text
left IC4 camera  --\
                   +--> shared IndexedReadOnlyFrameQueue
right IC4 camera --/                 |
                                    v
                         central FrameSyncThread
                                    |
                   +----------------+----------------+
                   |                |                |
                   v                v                v
              Varjo output      ImGui output    calibration /
                                                recording output
```

各出力は同じIC4Ext FramePool上のimmutable GPUテクスチャを`ReadOnlyFrame`として参照します。同期出力を追加しても、IC4Ext側でカメラテクスチャの出力別コピーは作成しません。

`ReadOnlyFrame` / `ReadOnlyFrameSet`はconsumerがGPU処理を終えるまで保持し、IC4ExtのFramePoolが使用中テクスチャを再利用しないようにします。

## Varjoへの表示

通常レンダリングは`XRSpace`の専用レンダースレッドで実行します。

```text
central FrameSyncThread
        |
        v
Varjo ReadOnlyFrameSet queue
        |
        v
XRSpace render thread
  +-- 最新の同期済み左右フレームを取得
  +-- left camera  -> Varjo Eye::Left
  +-- right camera -> Varjo Eye::Right
  +-- HLSL constantsを反映
  +-- Plane render / Varjo submit
  +-- rendered_frames.csv用データをlogger queueへ投入
```

Varjo表示用の`StereoDisplayTextureRing`は、共有カメラテクスチャをconsumer所有の表示テクスチャへコピーします。このコピーはIC4Extのfan-out用コピーではなく、Varjoへ安定した表示リソースを渡すための表示consumer内部処理です。コピー元のIC4Ext共有テクスチャ自体は遷移させず、FramePool leaseをGPU完了まで保持します。

## ライブキャリブレーションの動的出力

IC4Ext v2では、ライブキャリブレーション開始時にカメラへ第2のコピー出力を追加しません。代わりに中央`FrameSyncThread`へ校正用の`ReadOnlyFrameSetQueue`を一時登録します。

```text
central FrameSyncThread
  +-- Varjo ReadOnly output
  +-- ImGui ReadOnly output
  +-- calibration ReadOnly output   <- calibration中だけ登録
```

校正終了時には校正用outputの供給を停止してchannelを閉じます。左右カメラのcapture workerとVarjo表示経路はそのまま動作します。

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
- CMake 3.25以上
- IC Imaging Control 4 SDK
- Varjo Native SDK / Varjo Runtime / Varjo HMD
- vcpkgのOpenCV `x64-windows`

## ビルド（CMD）

IC4Ext v2へ依存バージョンが変わっているため、以前のビルドディレクトリを削除してください。

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

左右カメラのシリアルIDは必須です。

```bat
out\build\default\Release\DualIC4VarjoApp.exe ^
  --dir "logs" ^
  --project experiment01 ^
  --left-serial "LEFT_CAMERA_SERIAL" ^
  --right-serial "RIGHT_CAMERA_SERIAL" ^
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

`--left-json-device-index` / `--right-json-device-index`はIC4 state JSON内部のエントリ選択用であり、物理カメラの選択には使用されません。物理カメラは常に`--left-serial` / `--right-serial`で選択します。

次の旧オプションはエラーになります。

```text
--left-device-index
--right-device-index
--left-unique-name
--right-unique-name
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
