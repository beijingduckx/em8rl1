# CZ-8RL1 Emulator

通販サイトなどで売られているEZ-USB FX2LP Development board と Windows PCを使って、実機のCZ-8RL1をエミュレートします   
X1 turboなどの背面 CMT 端子に接続して、CZ-8RL1の代わりに使います

# 材料
1. Windows PC (64bit)
1. EZ-USB FX2LP Development board  ([例](https://www.amazon.co.jp/ez-usb/s?k=ez-usb))
1. 7pin DINコネクタ  ([CMTケーブルがある場合の例](https://www.marutsu.co.jp/pc/i/19050/))
1. 10kΩ抵抗 2本、配線、EZ-USB Development board と PCとを接続する USBケーブルなど

# 起動
1. EZ-USBが、WinUSBドライバで認識されるようにWindowsを設定します
1. 回路図にしたがって、DINプラグと抵抗とEZ-USBとを接続します  (注: RDY1とRDY0が逆に印刷されているボードがあります)
2. X1 背面の CMT 端子に DINプラグを接続します。
3. EZ-USBのUSB端子とPCとを接続します
5. `em8RL1.exe`を実行します
1. X1 の電源を投入します

# 使い方
- `File -> Set Tape..`で、カセットテープイメージ (*.tapファイル) を選択します
- `File -> Eject` で、セットされたテープイメージをイジェクトします
- カセットテープイメージがセットされている場合は、早送りや巻き戻しなどのボタンが表示され、操作が可能です

# 設定について
通常は設定を変更する必要はないと思いますが、ロードやセーブがうまくいかないときに
設定を変更してみてください

- `Settings -> Alt. 44kHz frequency`   
EZ-USBは 44.1kHz きっかりを生成できないので、2種類の周波数を設定できるようにしています  
この設定は、もう一つの周波数を設定します サンプリング周波数が44.1kHz、22.05kHzのテープイメージを使うときのみ有効です

- `Settings -> Bit conversion on save`  
セーブする際に、X1 から出力された波形を`em8RL1.exe`内部でX1標準ビットフォーマットとして
0, 1 解釈をし、その解釈結果をX1標準ビットフォーマットでテープイメージに書き込みます  
もともとは、サンプリング周波数が低いテープイメージにもセーブ可能とするための機能です  
サンプリング周波数が32kHz未満のテープイメージにセーブする場合は、設定に関わらずこのビット変換が有効になります   
この設定が無効の場合は、X1から出力された波形をサンプルしたビット値をテープイメージに書き込みます (サンプリング周波数が32kHz以上のテープイメージのみ)

# セーブについて
- セーブする場合は、データの破損を防ぐため、事前にテープイメージのバックアップをとっておいてください  
  (不具合により正しくセーブされなかったり、テープイメージを破損する可能性があります)
  
- Bit conversion機能を有効にした場合  
ゲームによっては正しくセーブできないことがあります 手元のソフトでは、トリトーンは正しくセーブできませんでした  
なお、APSS用のブランク書き込み中はカウンタが進みません (良い解決策が思いつかず)

- Bit conversion 機能を無効にした場合  
ゲームによってはサンプリング周波数32kHzのテープイメージでは正しくセーブできないことがあります (同じくトリトーン)   
このような時は、サンプリング周波数44.1kHz以上のテープイメージにセーブしてください  
  X1標準フォーマットでセーブするプログラムは、32kHzのテープイメージでもエラーなく書き込めるようです  
  (が、可能な限りベリファイしてください)  

- テープ長を超えてセーブすることはできません (イメージを自動的に伸長することはありません)

# テープイメージについて
- 旧型式、新形式の TAP ファイルに対応しています
- 新形式の場合、`Eject`時にテープの状態を保存します
- サンプリング周波数 48kHz, 44.1kHz, 32kHz, 22.05kHz, 8kHz のイメージでテストしています  
  その他8kHzの整数倍であれば正常に読み取り・書き込みできると思いますが、動作確認していません
- 旧型式の場合、イメージファイルに読み取り専用属性を付与することで、テープの消去防止爪を折った(書き込み禁止)状態とすることができます  
  新形式でも、この方法で消去防止爪を折った状態を作ることができますが、テープの状態保存はされません
- 旧型式を新形式に変換したり、新形式での消去防止爪のフラグを設定する機能は持っていません (エミュレータ等をご利用ください)


# 動作について
- CZ-8RL1 の全ての動作を解析・再現しているわけではありません  
  X1からのコマンドに対する応答やタイミングが異なる箇所が多々あると思います
- 実時間で動作するデータレコーダーの動作をエミュレートする関係で、本プログラムの動作にはある程度のリアルタイム性が必要となっています  
特に、X1からのコマンドとテープイメージとのタイミングのずれを最小限とするため、USBで1回に転送するテープイメージのデータ量を
少なくしています  
 この関係で、PCの処理が滞った場合に、リード/セーブエラーとなることがあります
- EZ-USBがハングすると、X1のサブCPUの動作が不正となることがあります (キーボードが効かなくなる・IPLリセットを押しても、`Please trun on the power SW slowly again`の表示となる など)  
  この場合、EZ-USBをリセットし、`em8RL1.exe`を立ち上げ直すことで解消されることがありますが、ダメな時はX1の背面のスイッチを一旦OFFにしてください
- 'USB Error'のダイアログが表示されることがあります この場合も、EZ-USBをリセットし、`em8RL1.exe`を立ち上げ直してください


# テスト環境
- X1 turboII
- Windows11 22H2


# 非保証
本リポジトリ内のプログラム、回路図は、正常に動作することを期待して作成していますが、正常な動作を保証しません。  
本リポジトリ内のプログラム・回路図を参照・利用したことにより生じた損害(X1が破損する、EZ-USBが破損する、PCが破損する、テープイメージが破損するなど)に対し、制作者は一切補償しません。  
特に、テープイメージへデータを書き込む機能があるため、事前にテープイメージのバックアップを取ってから、利用してください。


# プログラムについて
- `firmware`フォルダのプログラムは、SDCCをインストールしPATHが通ったPowerSehll環境で`./compile.ps1`とすればコンパイルできます
- Windows側のプログラムのコンパイルは、VisualStudio 2022, SDL2開発ライブラリ、libusb、ImGui (https://github.com/ocornut/imgui) が必要です  
 (SDL2ライブラリは、プロジェクトディレクトリ内にincludeディレクトリ、libディレクトリを作成してその中に入れます)