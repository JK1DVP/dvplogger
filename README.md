# dvplogger
field companion for ham radio operator
手のひらサイズのアマチュア無線家のためのフィールド支援ツール、コンテストロガーです。

Release にマニュアル、ログ変換プログラム、内蔵Bluetooth モデム用プログラム(Arduino)を置いています。
また、source からのビルド方法を下記に記します。

# License
下記の通りGPLv2 or laterです。
/* Copyright (c) 2021-2026 Eiichiro Araki
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
Release で配布されているバイナリイメージを書き込む方法は[esptool](https://github.com/espressif/esptool) をインストールした上で、bootloader.bin partition-table.bin dvplogger.bin をディレクトリに置き、

python esptool.py -p シリアルポート -b 460800 --before default_reset --after hard_reset --chip esp32  write_flash --flash_mode dio --flash_size detect --flash_freq 40m 0x1000 bootloader.bin 0x8000 partition-table.bin 0x10000 dvplogger.bin
`
で書き込めるはずです。
subcpu への書き込みは、Hamfair 2025以降の頒布ハードウェアではいずれも基板にアクセスせずともできるようになっていますが、それ以前の場合は下記の通り、少々改造が必要です。
<img width="782" height="605" alt="image" src="https://github.com/user-attachments/assets/fad1a7f8-5d5b-4aea-9dcf-ff3e408ce474" />
のように、２本の線を10pin コネクターと秋月モジュールの間に接続を行ってください。


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

その上で、

bash build-all.sh

すれば、２つの異なるハードウェア用のバイナリがbinaries/Wide, miniの下にできます。<br>
書き込みは、バイナリ配布と同様に、binariesの下にできているファイルをesptoolを使ってもできますし、<br>
idf.py を使い、
idf.py -B build-main-hw1 flash monitor (mini版の場合) または、<br>
idf.py -B build-main-hw3 flash monitor (mini版の場合) <br>
としても、行えます。

同様に、サブCPUのプログラムもapp0.bin , bootload.bin, parttio.bin, spiffs.binのようにできています。<br>
サブCPUのプログラムはdvplogger のWebサーバーにアクセスし、DVPloggerのSDメモリにアップロードをしてください。
そのうえで、dvploggerのターミナル接続(idf.py monitorなどでやると良いでしょう）から、<br>
flashersd app0 boot part spiffs [Enter]<br>
とコマンドを打つことで、サブCPUへのflash書き込みができるようになっています。

プログラムを書き換えた場合には、<br>
restart_dvplogger<br>
コマンドで再起動することをおすすめします。
