# DVPlogger Release Notes
作成日: 2026-10-07
## 1. Mini (HW1) のDUPECHECK / CALLHISTメモリ最適化
> Mini (HW1)のDUPECHECK/CALLHISTメモリ使用量を削減し、大規模なCALLHIST・DUPEデータをより安定して扱えるよう改善しました。DUPE判定の内部データ構造も圧縮し、MAIN/SUBCPU間のメモリ配置を見直しています。
## 2. Web / Networkの低メモリ化
> Mini (HW1)でWeb画面やログ出力を使用した際のメモリ不足・再起動を抑制しました。Webレスポンス、Network処理、SDアクセスで使用する一時メモリを共有・streaming化し、内部RAMのpeak使用量を削減しています。
## 3. QSO log読み出し・各種export処理の再設計
> READQSO、DUMPQSO、ADIF、CSV、JARL log、Cabrillo等のログ出力処理を再設計しました。大きなQSOログをbatch/background処理することで、特にMini (HW1)でのメモリ使用量と操作応答性を改善しています。
## 4. Cabrillo出力の改善
> Cabrillo出力時に対象Contestを選択できるようにしました。Dual Contest等で同一QSOログに複数ContestのQSOが記録されている場合にも対応します。
## 5. ZMERGE / ZMERGENEWの性能・安定性改善
> ZMERGE/ZMERGENEWの処理速度と安定性を改善しました。大きなQSOログのmerge時のメモリ使用量を削減し、QSO IDの重複・競合処理も強化しています。
## 6. QSO log処理のsingle-writer化・長時間job改善
> QSO logへのアクセスを整理し、長時間のREADQSO/export処理中でもDVPlogger全体の操作を継続しやすくしました。
## 7. Satellite / AOS / TLE処理の改善
> Satellite Web画面のAOS表示を改善しました。またTLE更新処理を見直し、download失敗時には既存の正常なTLEを保持し、更新が正常完了した場合だけ新しいSatellite listへ切り替えるようにしました。
## 8. Version / build情報
build情報でfirmwareを識別する方式へ変更しました。versionコマンド, /status欄で表示できます。
## 9. No polling オプションをRig設定に追加
NP:1 をリグ設定に書くことで、DVPloggerからの定期的なリグへのpolling をしないようにできます。ICOMでのトランシーブ動作設定を前提にした機能です。
