# dvplogger
field companion for ham radio operator
手のひらサイズのアマチュア無線家のためのフィールド支援ツール、コンテストロガーです。

Release にマニュアル、ログ変換プログラム、内蔵Bluetooth モデム用プログラム(Arduino)を置いています。
また、source からのビルド方法を下記に記します。

# License
下記の通りGPLv2 or laterです。
/* Copyright (c) 2021-2025 Eiichiro Araki
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see http://www.gnu.org/licenses/.
*/

## binary のesp32 への書き込み
Release で配布されているバイナリイメージを書き込む方法は[esptool](https://github.com/espressif/esptool) をインストールした上で、bootloader.bin partition-table.bin jk1dvplog.bin をディレクトリに置き、

python esptool.py -p シリアルポート -b 460800 --before default_reset --after hard_reset --chip esp32  write_flash --flash_mode dio --flash_size detect --flash_freq 40m 0x1000 bootloader.bin 0x8000 partition-table.bin 0x10000 jk1dvplog.bin
`
で書き込めるはずです。

## source からのビルド方法
本システムは、esp-idf v4.4.7 でビルドしています。
Windows でもできるはずですが、下記記述はUbuntu Linux での開発環境を参照して記述しています。

https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32/get-started/index.html
に従い、esp-idfをセットします。
本リポジトリをclone するとdvplogger, dvplogger_ext とできているかと思いますが、それぞれメインCPU,サブCPUで走るプログラムのツリーです。

また、本システムはarduino-esp32 v2.0.17 をcomponent として使っています。
https://github.com/espressif/arduino-esp32/tree/2.0.17
などに従い、dvplogger/components/arduino/にArduino-esp32 v2.0.17をcloneしてください。
参考：

git clone -b 2.0.17 --recursive https://github.com/espressif/arduino-esp32 ./components/arduino

ただし、dvploggerでは、arduino-esp32のライブラリを追加かつ変更していますので、components/arduino/libraries/の下を本リポジトリの内容で上書きをしてください。（こんなやり方で良いか不明・・・）

その上で、メインCPU用のプログラムを

cd dvplogger<br>
idf.py build flash monitor<br>

すれば、ビルドして書き込みまでできるはずです。

[注意] 本システムのメインCPUのプログラムはmini 版とwide版でビルドを分けることが必要です。

ビルド前に

dvplogger/main/decl.h に<br>
\#define JK1DVPLOG_HWVER<br>
の記述がありますので、mini版の方は<br>
\#define JK1DVPLOG_HWVER 1<br>
、Wide版の方は<br>
\#define JK1DVPLOG_HWVER 3<br>
とコメントアウトを外してください。

サブCPUのプログラムの方は、同様に<br>
cd dvplogger_ext<br>
idf.py build<br>
することにより、dvplogger_ext にapp0.bin , bootload.bin, parttio.bin, spiffs.bin のそれぞれシンボリックリンク先の実態ができていると思います。
これをdvplogger のWebサーバーにアクセスし、DVPloggerのSDメモリにアップロードをしてください。
そのうえで、dvploggerのターミナル接続(idf.py monitorなどでやると良いでしょう）から、<br>
flashersd app0 boot part spiffs [Enter]<br>
とコマンドを打つことで、サブCPUへのflash書き込みができるようになっています。

また、古い版のサブCPUプログラム（最低限の機能を実装）はSDへのアップロードなしに、flasher コマンドで書き込めるようにしておきましたので、不都合がある場合は、試してみてください。

プログラムを書き換えた場合には、<br>
restart_dvplogger<br>
コマンドで再起動することをおすすめします。
