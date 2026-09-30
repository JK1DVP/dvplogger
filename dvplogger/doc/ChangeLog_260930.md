# DVPlogger ChangeLog

## Ham Fair 2026 頒布版 → 現在版

> **日本語版・第3稿（2026-09-30）**\
> ハムフェア2026で頒布した版以降に追加・変更した内容を、Gitのcommit
> messageだけでなく、実際のソースコード差分を中心に整理したものです。\
> 公開済み機能に限定せず、現在のソースに入っている開発機能・内部改善も含めています。途中で試しただけで最終ソースから消えた診断コードや実験は原則として除外しています。

------------------------------------------------------------------------

# 主な変更

ハムフェア頒布版以降、特に次の部分を大きく拡張しました。

-   USB CAT /
    USBキーイングを拡張し、IC-705、QMX、Yaesu系USB接続などへの対応を改善
-   USB Hubの抜き差し・再接続を大幅に安定化
-   RTTY運用機能を拡張し、USB
    FSK、DTR/RTS、リグ別極性・タイミングなどに対応
-   Dual Contest機能を追加
-   DUPECHECK / CALLHIST / MAKEDUPEを再設計し、Mini (HW1) / Wide
    (HW3)それぞれのメモリ構成に最適化
-   CQ WW RTTYに対応
-   Cabrillo出力を追加・改善
-   SWR表示とオートチューナー制御を追加
-   IC-9700を中心にSatellite運用を大幅改善
-   Webに現在位置設定ページを追加
-   CALLSTACKを追加
-   Web処理・ネットワーク処理を省メモリ化し、特にMini
    (HW1)での安定性を改善
-   Band切替、RIT/XIT、Filter、AGC、FTX-1などのリグ制御を拡張

以下、簡単に試せる操作方法も含めて説明します。

------------------------------------------------------------------------

# Contest / Logging

## Dual Contest

Contest欄へ `MAIN,SUB` の順でカンマ区切り入力します。例：`UserAKINT,UserFUKOUT`。設定したMain/Subの組は保存され、起動時にも復元されます。`Alt-Shift-C` でMain/Subを切り替えます。

Contest欄へ `UserAKINT` のようにMainだけを入力した場合は、明示的なSingle Contest指定になります。この場合、以前のSub Contestは消去され、次回起動時にSubが勝手に復活することはありません。

受信Contest Numberが2つあるQSOでは、Exchange欄へ `13,25` のように入力します。左が現在のMain Contest、右がSub Contestです。送信Contest Numberは1つの欄へ2個を毎回入力する方式ではなく、各Contest自身に設定されたSent EXCHを使います。Main側QSOにはMainのSent EXCH、Sub側QSOにはSubのSent EXCHが入ります。

------------------------------------------------------------------------

## User Contest

User Contestの読み込み・名前の正規化・選択処理を改善しました。

Dual Contestでも通常のUser Contest定義をそのまま利用できます。

Contest名は従来どおりContest欄から指定できます。

------------------------------------------------------------------------

## CQ WW RTTY

`CQWWRTTY` を組み込みContestとして追加しました。

交換ナンバーは内部的に、

``` text
ZONE/QTH
```

として扱います。

例：

``` text
25/DX
03/CA
04/ON
04/NWT
05/PEI
```

DX局では、

``` text
25
```

だけ入力しても、

``` text
25/DX
```

として扱います。

W/VE局ではState/Province/Territoryをチェックします。

例：

``` text
AJ6UV  03/CA
```

はOKですが、JA局に対して、

``` text
JA1xxx  03/CA
```

と入力すると不正なQTHとして扱われます。

JAなどW/VE以外では、

``` text
25/DX
```

のように`DX`を使用します。

Alaska / HawaiiもCQWWRTTYではQTH欄を`DX`として扱います。

### Dual Contestとの区切り

`,` はDual Contest用、

`/` はCQWWRTTYのZone/QTH用です。

したがって将来的な複合入力も区別できます。

------------------------------------------------------------------------

## Cabrillo出力

Cabrillo 3.0出力はWebから利用できます。Webのlog出力からCabrilloを取得します。CQWWRTTYでは内部の `25/DX`、`03/CA` をCabrillo出力時に `25 DX`、`03 CA` のように分離します。

------------------------------------------------------------------------

# DUPECHECK / CALLHIST / MAKEDUPE

## Portable callsignの扱い

Portable suffixの扱いをDUPECHECK/CALLHISTで改善しました。末尾の `/1`～`/9`、`/P`、`/M`、`/MM`、`/AM`、`/QRP` はbase callsignとの比較に使えるよう正規化します。一方、`F/JA1ABC`、`KH0/JA1ABC` のようなprefix operationはそのまま保持します。


この部分はハムフェア版からかなり大きく変更しています。

## MAKEDUPE

QSOログからDUPEデータベースを作り直す処理を、従来の長時間ブロックする方式からincremental/non-blocking方式へ変更しました。

起動時、Contest変更時、手動MAKEDUPEで共通の再構築経路を使います。

### 試してみる

CALLSIGN欄に、

``` text
MAKEDUPE
```

と入力してEnterします。

処理中でもDVPlogger全体を長時間止めないように変更されています。

DUPEデータをクリアする場合は、

``` text
DUPERESET
```

も利用できます。

------------------------------------------------------------------------

## DUPEデータベース容量

DUPE容量を設定できるようにしました。

### 試してみる

CALLSIGN欄に例えば、

``` text
DUPEMAX2500
```

と入力してEnterします。

現在のソースでは、

``` text
DUPEMAX200 ～ DUPEMAX10000
```

の範囲を受け付けます。

設定値は保存され、変更後はMAKEDUPE再構築が予約されます。

------------------------------------------------------------------------

## CALLHIST

CALLHISTの検索方式とメモリ利用を大きく変更しました。

CALLHISTファイルの指定は、

``` text
CALLHIST<file>
```

を使用します。

配置を明示的に変更するコマンドとして、

``` text
CALLHISTMAIN
CALLHISTSUB
```

があります。

### Mini (HW1)

HW1にはPSRAMがないため、MAIN側では、

-   callsign/indexを内部RAM
-   CALLHIST本体をSD

に置くcompactな方式を使用できます。

これにより、大きなCALLHISTを全部内部RAMへ展開せずに検索できます。

SUBCPU側を使う方式もfallbackとして利用できます。

### Wide (HW3)

HW3ではMAIN側PSRAMを利用できるため、大きなCALLHIST/DUPEデータをより積極的にメモリ上へ置けます。

------------------------------------------------------------------------

# CALLSTACK

## 複数局の入力と選択

`CALLSTACKON` で有効にし、CALLSIGN欄へ `JA1AAA,JA1BBB,JA1CCC` のようにカンマ区切りで入力します。QSOしたいcallsignの文字上へカーソルを移してEnterを押すと、その1局が選択されます。カンマの位置でEnterした場合は右側の局を選びます。

例：`JA1AAA,JA1BBB,JA1CCC` の `JA1BBB` 上でEnterすると `JA1BBB` が通常の1局分のCALLSIGNとして処理されます。QSO確定後はその局だけがstackから除かれ、`JA1AAA,JA1CCC` が残ります。QSOを確定せず戻った場合は元のstackを復元します。CALLSIGN欄の `;` でも、stack中はカーソル位置の1局を選んでから従来の `;` 動作へ進みます。

------------------------------------------------------------------------

# RTTY

ハムフェア版以降、RTTYはかなり大きく拡張しました。

-   FSKキーイング
-   USB経由FSK
-   DTR / RTS制御
-   リグごとの極性
-   Normal / Reverse
-   PTT lead/tail
-   RTTY message
-   exchange自動挿入
-   RTTY表示・terminal処理

などを改善しています。

## RIG Settings：CW / FSK / PTT

WebのRIG SettingsでCWとRTTY FSKを独立設定できます。

`CW:0..4` / `FSK:0..4` の意味は、`0=LED/GPIO, 1=KEY1, 2=KEY2, 3=USB DTR, 4=USB RTS` です。`FSK:` を省略すると互換性のためCW portをFSKにも使います。

例：`CW:4,FSK:3,PTT:2,RP:0` は、CW=USB RTS、RTTY FSK=USB DTR、PTT=CAT/CI-V追加使用、RTTY polarity=normalです。MARK/SPACEが逆なら `RP:1` を指定します。

## CP2105を使うYaesu USB接続

CP2105 driver内部の説明は割愛します。利用面ではYaesu dual-port USB interfaceに対応し、Enhanced COM側をCAT、Standard COM側をCW/FSK等のkeyingに使用します。FTX-1では `P:-1` を指定するとUSB CAT（CP2105 Enhanced port）を使用します。実装上はYaesu New CAT / Yaesu Old CAT / FT-817/818系のUSB transportでCP2105経路を利用できます。

------------------------------------------------------------------------

# RIT / XIT / Filter / AGC

## RIT / XITと↑↓キー

`F9=RIT ON/OFF`、`F10=XIT ON/OFF`、`F11=RX Filter/WIDTH`、`F12=AGC` を追加しました。RITまたはXITが調整対象になっているとき、従来の↑/↓による通常周波数操作は `↑=+20 Hz`、`↓=-20 Hz` のRIT/XIT offset調整に変わります。RIT/XIT調整中でなければ従来どおり通常の周波数調整です。

------------------------------------------------------------------------

# Band / Mode / Scope

Band切替時の周波数・Mode・Filter・CQ/S&P状態の復元を見直しました。CAT応答が遅れて戻った場合に、古い周波数を現在値と誤認して戻してしまう問題も抑制しています。

通常のMode切替は `Alt-M`、対応RIGでのSpectrum Scope再センタリングは `Alt-'` です。

Satellite operation中は通常運用と異なり、選択した衛星のSatellite DBに登録されたuplink/downlink Modeを基準にRIG modeを設定します。FM衛星ではFM、linear transponderではDBのmodeを基準にUSB/CW等を設定します。Satellite中の `Alt-M` もこのmode情報を考慮して動作します。

------------------------------------------------------------------------

# SWR表示 / AUTOTUNER

## Alt-T / Tuner

Ham Fair頒布版では `Alt-T` はPTT toggleでしたが、現在版ではTUNE開始/停止へ変更しました。RIG Settingsの `T:` で制御先を指定します。

`T:0`=Tuner制御なし（legacy Alt-T）、`T:1`=KEY1、`T:2`=KEY2、`T:3`=USB DTR、`T:4`=USB RTS、`T:5`=RIG内蔵TunerをCAT/CI-Vで制御、です。外部contact保持時間は `TH:1500` のようにmsで、auto tune thresholdは `SWR:200` のようにSWR×100で指定します。

------------------------------------------------------------------------

# USB / CAT

USB CAT / USB keying、USB Hub経由の認識・再接続を大きく見直しました。IC-705、QMXに加え、Yaesuのdual-port USB interfaceではCP2105経路を利用できます。CP2105ではEnhanced COM側をCAT、Standard COM側をCW/FSK等のkeyingとして扱います。FTX-1では `P:-1` でUSB CATを選択できます。

## IC-705 / QMX

USB CDC-ACM経由でのCATを改善しました。

特に、

-   IC-705
-   QMX

について、USB接続・選択・再接続処理を繰り返し見直しています。

リグを切り替えたときに、前のUSB
sessionの古いCAT要求が新しいリグへ流れないようにしました。

------------------------------------------------------------------------

## USB Hubの抜き差し

DVPlogger本体だけでなく、同梱しているUSB Host
Shieldライブラリ側も変更しています。

-   Hub enumeration
-   nested Hub
-   connect/disconnect debounce
-   per-port reset
-   stale child device release
-   reconnect
-   IC-705再接続
-   QMX再接続

などを改善しました。

USB
Hubを使っている場合は、ハムフェア版より抜き差し・再認識に強くなっています。

------------------------------------------------------------------------

## IC-705 USB Audio

IC-705のUSB Audio
Class/PCM2901についても実験的な受信処理を追加しています。

descriptor解析、interface選択、PSRAM captureなどのコードを含みます。

これは現時点では通常のContest
logger機能というより、開発・実験機能としての位置づけです。

------------------------------------------------------------------------

# IC-9700 / Satellite

## Satellite：SO1R/SO2Rから独立

Satellite operationはSO1R/SO2Rのradio modeから独立しました。そのため `Ctrl-5` は現在 **SO1R / SO2Rの切替だけ**を行い、Satellite ON/OFFには使いません。

WebのSatellite画面には衛星選択のselect/button、Satellite mode ON/OFF、Satellite database一覧の「選択」ボタンなどを追加しました。選択衛星のuplink/downlink modeをSatellite DBから読み、FM衛星ではFM、linear transponderではDBのmodeを基準にUSB/CW等へ設定します。Satellite中の `Alt-M` もこのmode情報を考慮します。IC-9700 native Satellite modeではMAIN=RX/downlink、SUB=TX/uplinkとして扱います。

------------------------------------------------------------------------

# 位置情報

## 現在位置のWebページ

Webに `/location` の「現在位置の設定」ページを追加しました。Home等からリンクがあります。緯度・経度直接入力、4/6文字Grid Locator、地図クリック、スマートフォンの現在位置取得が使えます。地図表示にはInternet接続が必要ですが、数値/Grid入力は地図なしでも使用できます。

------------------------------------------------------------------------

# Web / Network / Memory

Mini (HW1)で特に問題になりやすかった内部RAM消費を大きく見直しました。

-   巨大なHTML/String生成を削減
-   Web responseをstreaming化
-   CSV/log生成時のpeak heap削減
-   AsyncTCP負荷削減
-   debug/Serial出力削減
-   CALLHISTをSD-backed化
-   DUPEをSUBCPUへ配置可能
-   PSRAMがあるHW3ではPSRAMを活用

などを行っています。

このため、同じ機能でもHW1とHW3で内部のdatabase配置が異なる場合があります。

------------------------------------------------------------------------

# QSO log / SWITCHLOG / ZMERGE

## SWITCHLOG

`LISTQSOFILE` でbackup QSO logを確認し、CALLSIGN欄で `SWITCHLOG003` のように入力すると `/qsobak.003` へ切り替えます。CALLSIGN欄では3桁指定です。Terminalでは `switchlog 3` のような指定も受け付けます。

## ZMERGE / ZMERGENEW

従来の `ZMERGE` に加え、メモリ負荷を抑えた新しいmerge protocolの `ZMERGENEW` を追加しました。CALLSIGN欄で `ZMERGENEW` と入力すると標準batch size 10で動作します。Terminalでは `zmergenew`、`zmergenew 10`、`zmergenew 1` が使用できます。Mini (HW1)などで大きなlogをmergeするときに特に有用です。

------------------------------------------------------------------------

# Serial File Transfer / YMODEM

Wi-Fi/Web経由のfile転送がうまくいかない場合の代替として、SerialからmicroSDへfileを転送できます。CALLHIST `.PCK/.SPC` や `spiffs.bin` / `SPIFFS.bin` 等を転送する用途を想定しています。

Serial consoleで `ymodem` を実行するとYMODEM-CRC受信待ちになります。Tera Termでは `File → Transfer → YMODEM → Send` から送信します。受信後はsizeとCRC32を再確認してから既存fileと置き換えます。`ymodem` はSerial console専用です。低レベル用途には `sdput <file> <size> <crc32>` もあります。

------------------------------------------------------------------------

# Bandmap / Cluster

BandmapとCluster受信も見直しました。

-   Band変更直後の古い状態
-   Spot選択時の周波数/mode
-   DUPE済局の再Spot
-   Skimmerから大量Spotが来る場合

などへの対応を改善しています。

特にClusterの1行ごとに同期的なSUBCPU
DUPE照会を行わないようにし、高レートのSpotでも操作性を落としにくくしています。

------------------------------------------------------------------------

# キーボード操作の変更

今回追加・変更した主な操作です。

| 操作 | 機能 |
|---|---|
| `Alt-Shift-C` | Dual ContestのMain/Sub切替 |
| `Alt-T` | 従来のPTT toggleからTUNE開始/停止へ変更 |
| `Alt-M` | Mode切替。Satellite時はSatellite DBのmodeを考慮 |
| `Alt-'` | Spectrum Scope再センタリング |
| `Alt-R` | Satellite VFO/RX-TX側切替（単一IC-9700等） |
| `F9` | RIT ON/OFF |
| `F10` | XIT ON/OFF |
| `F11` | RX Filter / WIDTH |
| `F12` | AGC |
| `↑ / ↓` | RIT/XIT調整中は±20 Hz、それ以外は通常の周波数調整 |
| `Ctrl-5` | SO1R / SO2R切替のみ。Satelliteとは独立 |
| `Alt-W` | 標準ではQSO全体をWipe |
| `Ctrl-W` | 標準では現在fieldをClear |

------------------------------------------------------------------------

# Wipeキーの変更

標準動作を `Alt-W = QSO全体をWipe`、`Ctrl-W = 現在編集中fieldだけClear` と整理しました。CALLSIGN欄で `WIPEKEYSWAP` を実行すると両者を入れ替え、`Alt-W = field Clear`、`Ctrl-W = QSO全体Wipe` にできます。設定状態は保存されます。

------------------------------------------------------------------------

# HELP表示

HELP自体は従来からあります。今回追加・変更したcommand/shortcutをHELP/HELPR表示へ反映しました。

------------------------------------------------------------------------

# その他の内部改善

ユーザーから直接見えない部分でもかなり変更しています。

-   起動時のblocking処理削減
-   CAT query sequence整理
-   stale CAT response対策
-   MAIN/SUBCPU間通信改善
-   SDアクセスとAsyncTCPの競合低減
-   watchdog対策
-   heap fragmentation対策
-   HW1/HW3別memory allocation
-   USB Host再接続処理
-   keyboard latency低減
-   diagnostic codeの整理
-   未使用USB Host経路の無効化

などです。

------------------------------------------------------------------------

# まず試すなら

1. Web Homeから「位置設定」を開き、Grid Locatorを設定する。
2. `F9`でRITをONにし、↑/↓で20 Hzずつoffsetが変わることを確認する。
3. `CALLSTACKON` → `JA1AAA,JA1BBB,JA1CCC` を入力し、カーソル位置の局をEnterで選ぶ。
4. Contest欄へ `UserAKINT,UserFUKOUT` のように入力し、`Alt-Shift-C` でMain/Subを切り替える。
5. Web RIG Settingsで `CW:` / `FSK:` / `T:` の設定を確認する。
6. Tuner設定済みRIGで `Alt-T` を押し、Tune動作を確認する。
7. Web Satellite画面から衛星を選び、Satellite modeをONにする。
8. `LISTQSOFILE` → `SWITCHLOGnnn` を試す。
9. Miniでは `ZMERGENEW` で低メモリmergeを試す。
10. Wi-Fi転送が難しい場合はSerial consoleで `ymodem` を実行し、CALLHIST等を転送する。
