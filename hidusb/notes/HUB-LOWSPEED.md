# ハブ経由の低速 HID デバイス(検討メモ)

状態: **未解決。コードは元に戻してある**(HIDUSB 本体には入っていない)。
実験した修正は `hub-workarounds.c.patch` / `hub-workarounds.h.patch`(現行ソースに当てると再現)と、
全文スナップショット `hidusb.c.hub-workarounds` / `hidusb.h.hub-workarounds` に残してある。

## 症状(ThinkPad 235 / OHCI / OSR2.1 USB サプリメント)

環境: バスパワーの USB ハブ `1A40:0101`(Terminus FE1.1s 系、USB 2.0 ハブ)。

| 接続 | デバイス | 結果 |
|---|---|---|
| 直付け | キーボード 17EF:608C(低速) | 正常 |
| ハブ経由 | マウス 046d:c52f(全速レシーバ) | 正常 |
| ハブ経由 | USB メモリ(全速) | 正常 |
| ハブ経由 | キーボード 17EF:608C(低速) | **割り込み IN が全部失敗、レポートが来ない** |

ハブ経由の低速キーボードのログ:

- 最初の `GET_DESCRIPTOR`(device / config)が `c000009c` で失敗することがある。再試行すると通る(250ms 間隔で 1〜数回)。
- `SELECT_CONFIGURATION`、`SET_IDLE`、`SET_REPORT`(LED)、`GET_DESCRIPTOR(report)` などコントロール転送は通る。
- 割り込み IN だけが毎回 `irp=c000009c urb=c0000005`(USBD_STATUS_DEV_NOT_RESPONDING)。
- 何度か抜き差しすると、`GET_DESCRIPTOR(device)` が 6 回全部失敗して列挙自体ができなくなる。

## 試した修正(すべて効果なし、元に戻した)

1. **記述子取得のリトライ**(250ms 待って device 6 回 / config 4 回)。
   列挙の失敗は救えるが、割り込み IN は直らない。ハブ経由で毎回起きる「最初の要求が失敗する」現象の対処としては有効だった。
2. **割り込みパイプの RESET_PIPE**(転送失敗後)。
   ポイント: OSR2.1 の USBD は URB の長さを機能コードごとに厳密に検査する。`URB_FUNCTION_RESET_PIPE`(0x1E)と
   `ABORT_PIPE`(0x02)は **0x14 バイト**(98DDK の `_URB_PIPE_REQUEST` は末尾に Reserved があって 0x18 → STATUS_INVALID_PARAMETER)。
   0x14 にするとリセット自体は成功したが、その後も転送は失敗し続けた。
3. **コントロール転送ポーリング**(`GET_REPORT` を 16ms ごとにタイマ + DPC で発行し、変化したら渡す)。
   `GET_REPORT` も `c000009c` で失敗(確率的に成功することもあり、最初の 1 回はレポートが読めた)。
   ハブ経由の低速デバイスは、コントロール転送自体も不安定。

## 調べて分かったこと

- OPENHCI.SYS(`usbsupp/usbsupp/WDMUSB` と `usbsupp/usbupd2`)の ED 生成(`0xf8c` の `shl ecx, 0xd`)では、
  速度ビット(ED の S ビット)は **デバイス単位の 1 フラグ**(構造体 +0x12 の bit0)から作られ、
  エンドポイントごとの差は無い。割り込みだけ速度が違う、という種類の設定ミスではない。
- USBD.SYS の URB 検査表(`.data` +0x3800、12 バイト/エントリ: ハンドラ, 期待長, setup 先頭 2 バイト, フラグ)で
  機能コード 0x00〜0x27 だけが有効(0x28 = GET_DESCRIPTOR_FROM_INTERFACE は c000000d)。
  `URB_FUNCTION_CONTROL_TRANSFER`(0x08)は期待長 0x50 で、PipeHandle=NULL でデフォルトパイプが使われる。
  ただしこれで `bmRequestType=0x81`(インターフェース宛)を送ると OHCI のルートハブ処理に落ちて `c0000185` になった。
- 低速デバイスがハブ配下に付くとき、OHCI はプリアンブル(PRE)付きで低速パケットを出す必要がある。
  コントロール転送は通るが割り込み転送だけ通らない、という振る舞いから、次のどれかを疑っている。
  - SOF 直後のフレーム先頭で出る周期転送(割り込み)の PRE を、このハブ(FE1.1s)が取りこぼす
  - OHCI ドライバのスケジュール(LS 閾値、周期リストの配置)の問題
  - バスパワーの電力不足(ただしマウスと USB メモリは動く)

## 参考: 自作ホストコントローラ(`u950w9x/`)での同じ症状

`u950w9x/README.md` #27 に、同じ現象の記録がある。「ハブ配下の低速デバイスは、ホストコントローラが
S=1 の ED に PRE を付けて送ることに頼る。ハブ配下で全速デバイスは動くのに低速だけ動かない
(ログに CC=5 DeviceNotResponding)なら、ハードウェアの限界」。ここのキーボードの症状
(割り込みが `urb=c0000005` = DEV_NOT_RESPONDING)と一致するので、ホストコントローラまたはハブ側の
制約の可能性が高い。

## 次に確認したいこと(未実施)

1. セルフパワーのハブ、別チップのハブ(USB 1.1 のハブ)で同じキーボードを試す。
2. ハブ経由で別の低速デバイス(別のキーボード、低速マウス)を試す。
3. `usbupd2` の USBD / OPENHCI(更新版)に入れ替えて同じ症状か確認する。
   現在入っているのが `usbsupp` と `usbupd2` のどちらかも確認する。
4. USB アナライザがあれば、ハブ配下の割り込み IN の PRE の有無を見る。
5. Win98 の USBD / OPENHCI(`w98usbdrv/`)で同じ構成を試すと、OHCI ドライバ側の問題か切り分けられる。

## 方針

ハブ経由の低速デバイスは今のところ非対応(直付けで使う)。
リトライだけは副作用がなく、列挙の安定化に効くので、必要になったら `hub-workarounds.c.patch` から取り出す。

## 追記: 再現(HID9、2 インターフェースのキーボード)

- 症状 ②(列挙もドライバも通るがレポートが来ない): これまでと同じ。`if0 polling interrupt IN: 0` の約 190ms 後に
  `poll failed if0 irp=c000009c urb=c0000005`(DEV_NOT_RESPONDING)。`SET_IDLE`、`SET_REPORT`(LED)のコントロール転送は成功。
  `GET_DESCRIPTOR`(report)は FROM_DEVICE が c000009c、control が c0000185 で取れず、組み込みのブートキーボード記述子で動かしている
  (hidtest は `report lengths input 9` まで出るが、レポートは届かない)。
- 症状 ①(差すと "Unknown device"、ログに何も出ない): HIDUSB がロードされる前、USBHUB / OPENHCI の列挙段階の失敗なので
  HIDUSB からは手が出せない(ハブへの何回かの抜き差しで `GET_DESCRIPTOR(device)` が失敗し続けるのと同じ現象と思われる)。
- 結論は変わらず「ハブ/ホストコントローラ側の制約」。HIDUSB で直せる範囲ではない。

## 追記: 切り分けの結果(結論)

同じハブ(1A40:0101)+ 同じキーボード(17EF:608C)で、
- Windows 11(USB 2.0 ホストコントローラ経由)では正常、
- 同じ ThinkPad 235 の Windows 98(標準の HID/USB ドライバ)でも**キーボードの列挙がおかしい**。

よって原因は HIDUSB / 自作の HID スタックではなく、TP235 の OHCI(ハードウェアまたは OPENHCI)とこのハブの組み合わせ。
**対応しない**(直付けで使う)。この件はここで閉じる。再開するのは、ハブ/ホストコントローラを替えられる場合のみ。
