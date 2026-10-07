# 盛岡市 気象情報ディスプレイ (PlatformIO版)

ESP32 Development Board (CH340C Type-C版) + GC9A01 (240×240 円形ディスプレイ) 用、
VS Code + PlatformIO ビルド環境のプロジェクトです。

内容・機能は Open-Meteo APIから盛岡市の気象情報を取得し、
現在の天気と今後4日間の予報を表示。
PlatformIOでは TFT_eSPI の設定をライブラリのファイルを書き換えずに `platformio.ini` の `build_flags` だけで完結できる。

## 1. 必要な環境

1. **VS Code** をインストール
2. VS Codeの拡張機能タブから **PlatformIO IDE** をインストール
   (インストール後、VS Codeの再起動が必要です)
3. インストール完了後、VS Code左側に PlatformIO のアリのアイコンが表示されます

## 2. プロジェクトの開き方

1. このフォルダ (`platformio.ini` があるフォルダ) を VS Code で「フォルダを開く」
2. PlatformIOが自動的にプロジェクトとして認識します
3. 初回はESP32用のツールチェーンとライブラリ (TFT_eSPI, ArduinoJson, U8g2) が
   自動でダウンロードされます (数分かかります・要インターネット接続)

## 3. Wi-Fiと場所を設定

`src/main.cpp` の冒頭にある以下の部分を、ご自宅のWi-Fi情報に書き換えてください。

```cpp
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
```
```cpp
// 盛岡市 (岩手県) の緯度経度
const float LAT = 39.7036f;
const float LON = 141.1527f;
const char* LOCATION_NAME_JP = "もりおか";
```

## 4. ビルド・書き込み・シリアルモニタ

VS Code下部のステータスバーにあるPlatformIOのアイコンから操作します。

- ✔ (チェックマーク): ビルドのみ
- → (右矢印): ビルド + ESP32への書き込み
- 🔌 (プラグ): シリアルモニタを開く (115200bps)

または、VS Codeのターミナルから以下でも実行できます。

```bash
pio run              # ビルド
pio run -t upload    # ビルド + 書き込み
pio device monitor    # シリアルモニタ
```

書き込み先のシリアルポートが自動検出されない場合は、`platformio.ini` 末尾の
`upload_port` / `monitor_port` のコメントを外し、ポート名を指定してください。
ポート名は `pio device list` コマンドで確認できます。

## 5. 配線

| Display | ESP32-Dev. |
|---------|----------|
| VCC | 3V3 |
| GND | GND |
| RST | IO 17 |
| CS  | IO 16 |
| DC  | IO 4 |
| SDA (MOSI) | IO 2 |
| SCL (SCLK) | IO 15 |

バックライト(BLK)ピンの配線が別にある場合は、`platformio.ini` の
`-DTFT_BL=-1` の部分を該当GPIO番号に変更してください。

## 6. TFT_eSPIの設定を変更したい場合

Arduino IDE版のように `User_Setup.h` を編集する必要はありません。
`platformio.ini` の `build_flags` セクションに書かれている `-D` から始まる
行がそのまま設定になっています。ピン配置やSPI速度、フォントの有効/無効などは
ここを編集してください。

## 7. ビルドがうまくいかない場合

- `pio run -t clean` でビルドキャッシュを削除してから再ビルドしてください。
- `platform = espressif32` の行にバージョンを固定していないため、
  通常は自動的に安定した最新版が使われます。もし特定バージョンで
  問題が出る場合は `platform = espressif32@6.9.0` のようにバージョンを
  明示的に固定することも可能です。
